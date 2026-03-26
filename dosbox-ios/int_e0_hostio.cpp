/*
 * int_e0_hostio.cpp — INT E0h host file transfer for R.COM / W.COM
 *
 * Hooks software interrupt E0h to provide file transfer between the
 * DOS guest and the host filesystem (the app's Documents directory
 * on iOS).  The DOS-side tools R.COM and W.COM issue these calls.
 *
 * AH=01  Open host file for reading   DS:DX → ASCIIZ path   CF=error
 * AH=02  Open host file for writing   DS:DX → ASCIIZ path   CF=error
 * AH=03  Read one byte                AL=byte  CF=1 on EOF
 * AH=04  Write one byte               DL=byte
 * AH=05  Close file                   AL=0 read, AL=1 write
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "int_e0_hostio.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "dosbox.h"
#include "cpu/callback.h"
#include "cpu/registers.h"
#include "hardware/memory.h"

class HostIO {
public:
	CALLBACK_HandlerObject callback;
	FILE *read_fp  = nullptr;
	FILE *write_fp = nullptr;
	std::string host_base_dir;

	~HostIO()
	{
		if (read_fp)  fclose(read_fp);
		if (write_fp) fclose(write_fp);
		// callback destructor calls Uninstall() automatically
	}
};

static HostIO *s_hostio = nullptr;

/* Read a NUL-terminated string from guest memory at DS:DX */
static std::string read_guest_string(uint16_t max_len = 255)
{
	PhysPt addr = SegPhys(ds) + reg_dx;
	std::string result;
	result.reserve(max_len);
	for (uint16_t i = 0; i < max_len; i++) {
		uint8_t ch = mem_readb(addr + i);
		if (ch == 0)
			break;
		result += static_cast<char>(ch);
	}
	return result;
}

/* Resolve a guest-provided path against the host base directory.
 * Relative paths are sandboxed under host_dir.
 * Absolute paths are rejected (the iOS sandbox is the last line of
 * defence, but we shouldn't rely on it). */
static std::string resolve_path(const std::string &guest_path)
{
	if (!s_hostio || s_hostio->host_base_dir.empty())
		return {};

	// Reject absolute paths and path traversal
	if (guest_path.empty() || guest_path[0] == '/')
		return {};
	if (guest_path.find("..") != std::string::npos)
		return {};

	return s_hostio->host_base_dir + "/" + guest_path;
}

static Bitu INT_E0h_Handler(void)
{
	if (!s_hostio)
		return CBRET_NONE;

	switch (reg_ah) {
	case 0x01: { // Open host file for reading
		std::string guest = read_guest_string();
		std::string path  = resolve_path(guest);
		if (path.empty()) {
			CALLBACK_SCF(true);
			break;
		}
		if (s_hostio->read_fp)
			fclose(s_hostio->read_fp);
		s_hostio->read_fp = fopen(path.c_str(), "rb");
		if (s_hostio->read_fp) {
			LOG_MSG("HOSTIO: Opened '%s' for reading", guest.c_str());
			CALLBACK_SCF(false);
		} else {
			LOG_MSG("HOSTIO: Cannot open '%s' for reading", guest.c_str());
			CALLBACK_SCF(true);
		}
		break;
	}
	case 0x02: { // Open host file for writing
		std::string guest = read_guest_string();
		std::string path  = resolve_path(guest);
		if (path.empty()) {
			CALLBACK_SCF(true);
			break;
		}
		if (s_hostio->write_fp)
			fclose(s_hostio->write_fp);
		s_hostio->write_fp = fopen(path.c_str(), "wb");
		if (s_hostio->write_fp) {
			LOG_MSG("HOSTIO: Opened '%s' for writing", guest.c_str());
			CALLBACK_SCF(false);
		} else {
			LOG_MSG("HOSTIO: Cannot open '%s' for writing", guest.c_str());
			CALLBACK_SCF(true);
		}
		break;
	}
	case 0x03: { // Read one byte
		if (!s_hostio->read_fp) {
			CALLBACK_SCF(true);
			break;
		}
		int ch = fgetc(s_hostio->read_fp);
		if (ch == EOF) {
			CALLBACK_SCF(true);
		} else {
			reg_al = static_cast<uint8_t>(ch);
			CALLBACK_SCF(false);
		}
		break;
	}
	case 0x04: { // Write one byte
		if (!s_hostio->write_fp) {
			CALLBACK_SCF(true);
			break;
		}
		fputc(reg_dl, s_hostio->write_fp);
		CALLBACK_SCF(false);
		break;
	}
	case 0x05: { // Close file
		if (reg_al == 0) {
			if (s_hostio->read_fp) {
				fclose(s_hostio->read_fp);
				s_hostio->read_fp = nullptr;
			}
		} else {
			if (s_hostio->write_fp) {
				fclose(s_hostio->write_fp);
				s_hostio->write_fp = nullptr;
			}
		}
		CALLBACK_SCF(false);
		break;
	}
	default:
		CALLBACK_SCF(true);
		break;
	}
	return CBRET_NONE;
}

void HOSTIO_Init(const char *host_dir)
{
	delete s_hostio;
	s_hostio = new HostIO();

	if (host_dir && host_dir[0])
		s_hostio->host_base_dir = host_dir;

	s_hostio->callback.Install(&INT_E0h_Handler, CB_IRET, "INT E0h Host File I/O");
	s_hostio->callback.Set_RealVec(0xE0);

	if (!s_hostio->host_base_dir.empty())
		LOG_MSG("HOSTIO: INT E0h ready, host dir: %s", s_hostio->host_base_dir.c_str());
	else
		LOG_MSG("HOSTIO: INT E0h ready, no host dir (transfers disabled)");
}

void HOSTIO_Destroy()
{
	delete s_hostio;
	s_hostio = nullptr;
}
