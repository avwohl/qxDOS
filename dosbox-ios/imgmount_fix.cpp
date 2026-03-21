// Fixed IMGMOUNT::Run() — avoids the double-free in the upstream code.
//
// The upstream creates a MOUNT on the stack and shares the `cmd` pointer.
// MOUNT::Run() → ChangeToLongCmd() can delete+replace `cmd`, leaving
// IMGMOUNT with a dangling pointer.  We fix this by transferring ownership.

#include "dos/programs/imgmount.h"
#include "dos/programs/mount.h"

void IMGMOUNT::Run(void)
{
	// Create a MOUNT command instance on the stack.
	// Its Program() constructor allocates its own cmd and psp.
	MOUNT mount_cmd;

	// Transfer our cmd to mount_cmd so MOUNT::Run() sees our arguments.
	// Delete mount_cmd's own cmd first to avoid a leak.
	delete mount_cmd.cmd;
	mount_cmd.cmd = this->cmd;
	this->cmd = nullptr; // prevent double-free if destructor runs early

	// Execute the unified mount logic
	mount_cmd.Run();

	// Take back whatever cmd mount_cmd has now (may be a new allocation
	// if ChangeToLongCmd fired, or our original pointer if it didn't).
	this->cmd = mount_cmd.cmd;
	mount_cmd.cmd = nullptr; // prevent mount_cmd destructor from freeing it
}

void IMGMOUNT::AddMessages()
{
	// Messages are handled in MOUNT::AddMessages
}

void IMGMOUNT::ListImgMounts(void)
{
	MOUNT m;
	m.ListMounts();
}
