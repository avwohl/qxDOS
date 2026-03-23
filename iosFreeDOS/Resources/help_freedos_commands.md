# FreeDOS Command Reference

Commands and utilities included with FreeDOS. This reference is adapted from the [FreeDOS Help project](https://github.com/FDOS/help).

---

## Internal Commands

These commands are built into COMMAND.COM (FreeCom) and are always available.

### CD / CHDIR

Change or display the current directory.

**Syntax:**

```
CD [drive:]path
CD ..
CD \
CD -
```

**Options:**

- `..` -- Change to the parent directory.
- `\` -- Change to the root directory.
- `-` -- Change to the last visited directory (if LAST_DIR feature is enabled).

CD does not change the currently selected drive. Use the drive letter (e.g. `D:`) to switch drives.

---

### COPY

Copy one or more files to another location.

**Syntax:**

```
COPY [/A | /B] source [+ source2 ...] [destination] [/V] [/Y | /-Y]
```

**Options:**

- `/A` -- ASCII mode (handles newline conversion and end-of-file character).
- `/B` -- Binary mode (copies file unchanged; this is the default for files).
- `/V` -- Verify that new files are written correctly.
- `/Y` -- Suppress prompting to confirm overwrites.
- `/-Y` -- Prompt to confirm overwrites.

The COPYCMD environment variable can preset `/Y` (use `SET COPYCMD=/Y`).

---

### DEL / ERASE

Delete one or more files.

**Syntax:**

```
DEL [/P] [/V] [drive:][path]filename
```

**Options:**

- `/P` -- Prompt for confirmation before deleting each file.
- `/V` -- Display all deleted files.

Wildcards (`*`, `?`) are supported. If a directory is specified, all files within it are deleted. DEL does not erase file contents from disk -- it marks entries as deleted. Use UNDELETE to attempt recovery.

---

### DIR

Display the contents of a directory.

**Syntax:**

```
DIR [drive:][path][filename] [/P] [/W] [/A[:attribs]] [/O[:sort]] [/S] [/B] [/L] [/Y]
```

**Options:**

- `/A:attribs` -- Filter by attributes: `R` read-only, `H` hidden, `S` system, `D` directories, `A` archive. Prefix with `-` to exclude.
- `/B` -- Bare format (filenames only).
- `/L` -- Display filenames in lowercase.
- `/O:sort` -- Sort order: `N` name, `E` extension, `S` size, `D` date, `G` group dirs first. Prefix with `-` to reverse.
- `/P` -- Pause after each screenful.
- `/S` -- Include subdirectories recursively.
- `/W` -- Wide list format (5 columns).
- `/Y` or `/4` -- Display 4-digit year.

The DIRCMD environment variable can preset options.

---

### MD / MKDIR

Create a new directory.

**Syntax:**

```
MD [drive:]path
```

In pure DOS the directory name must not be longer than 8 characters. If you type `MD dirname` without a path, the directory is created as a subdirectory of the current directory.

---

### RD / RMDIR

Remove an empty directory.

**Syntax:**

```
RD [drive:]path
```

RD only removes empty directories. Remove all files (including hidden ones) and subdirectories first. Use DELTREE to remove a directory tree in one step.

---

### REN / RENAME

Rename a file or directory.

**Syntax:**

```
REN [drive:][path]oldname newname
```

The destination must not contain a path -- the file is renamed in place. Wildcards in the destination are replaced by corresponding characters from the source name.

---

### TYPE

Display the contents of a text file.

**Syntax:**

```
TYPE [drive:][path]filename
```

Use `TYPE filename | MORE` to page through long files.

---

### DATE

Display or set the system date.

**Syntax:**

```
DATE [/D] [date]
```

**Options:**

- `/D` -- Display date only, do not prompt for a new date.

Date format depends on the country setting. For US format: `MM/DD/YYYY` or `MM-DD-YYYY`.

---

### TIME

Display or set the system time.

**Syntax:**

```
TIME [/T] [time]
```

**Options:**

- `/T` -- Display time only, do not prompt for a new time.

Time format: `HH:MM[:SS[.hundredths]]`. AM/PM modifiers are accepted.

---

### VER

Display FreeDOS version information.

**Syntax:**

```
VER [/R] [/W] [/D] [/C]
```

**Options:**

- `/R` -- Show kernel and COMMAND.COM version numbers.
- `/W` -- Show warranty information.
- `/D` -- Show distribution information.
- `/C` -- Show contributor acknowledgements.

---

### VOL

Display the disk volume label and serial number.

**Syntax:**

```
VOL [drive:]
```

Each drive can have its own volume label (up to 11 characters), set with LABEL or FORMAT /V.

---

### CLS

Clear the screen and reset character colors to white on black.

**Syntax:**

```
CLS
```

---

## Batch File Commands

Commands for writing batch (.BAT) files. Most of these are built into COMMAND.COM.

### ECHO

Display messages or turn command echoing on/off.

**Syntax:**

```
ECHO [ON | OFF]
ECHO [message]
ECHO.
```

**Usage:**

- `ECHO ON` / `ECHO OFF` -- Enable or disable display of each command line as it executes.
- `ECHO message` -- Print a message to the screen.
- `ECHO.` -- Print a blank line (no space between ECHO and the dot).
- `@ECHO OFF` -- The `@` prefix suppresses display of the ECHO command itself. Typically placed on the first line of a batch file.

Use `>` to redirect output to a file: `ECHO text > file.txt`. Use `>>` to append.

---

### FOR

Run a command for each item in a set.

**Syntax:**

```
FOR %variable IN (set) DO command
FOR %%variable IN (set) DO command
```

Use `%` on the command line and `%%` in batch files. The variable is a single character and is case-sensitive. The set can contain filenames (with wildcards) or literal values separated by spaces or commas.

---

### GOTO

Jump to a labeled line in a batch file.

**Syntax:**

```
GOTO label
```

Labels are defined as `:label` on a line by itself (colon in column 1). If a label appears more than once, the first occurrence is used. Combine with IF for conditional branching.

---

### IF

Conditionally execute a command.

**Syntax:**

```
IF [NOT] EXIST filename command
IF [NOT] ERRORLEVEL number command
IF [NOT] string1==string2 command
IF /I string1==string2 command
```

**Conditions:**

- `EXIST filename` -- True if the file exists. Wildcards supported.
- `ERRORLEVEL number` -- True if the last program's exit code is greater than or equal to `number`.
- `string1==string2` -- True if the strings match (case-sensitive). Use `/I` for case-insensitive comparison.
- `NOT` -- Negates the condition.

Tip: Quote strings to handle spaces: `IF "%1"=="" GOTO usage`.

---

### PATH

Display or set the search path for executables.

**Syntax:**

```
PATH
PATH [=] dir1;dir2;dir3
PATH ;
```

**Usage:**

- `PATH` with no arguments displays the current search path.
- `PATH dir1;dir2` sets the path. The current directory is always searched first.
- `PATH ;` clears the search path.
- Append to existing path: `PATH %PATH%;C:\NEWDIR`

---

### PAUSE

Suspend batch file execution and wait for a keypress.

**Syntax:**

```
PAUSE [message]
```

Displays "Press any key to continue..." or the specified message. Use `PAUSE > NUL` to pause without displaying any prompt.

---

### SET

Display, set, or remove environment variables.

**Syntax:**

```
SET [variable[=[value]]]
SET /P variable=[prompt]
SET /E variable=command
```

**Options:**

- No arguments: display all environment variables.
- `SET VAR=value` -- Set variable VAR to value.
- `SET VAR=` -- Remove the variable.
- `/P` -- Prompt the user for a value.
- `/E` -- Set variable to the output of a command.
- `/U` -- Convert value to uppercase.
- `/C` -- Preserve case of the variable name (default uppercases it).

Use `%VAR%` to reference a variable's value in commands and batch files.

---

### CHOICE

Prompt the user to make a selection and set ERRORLEVEL.

**Syntax:**

```
CHOICE [/B] [/C:choices] [/N] [/S] [/T:c,nn] [text]
```

**Options:**

- `/B` -- Beep when prompting.
- `/C:choices` -- Specify allowed keys (default: YN). Can be letters or digits.
- `/N` -- Do not display the choice keys in brackets.
- `/S` -- Make choices case-sensitive.
- `/T:c,nn` -- Default to key `c` after `nn` seconds.

ERRORLEVEL is set to the position of the key pressed (1 for the first choice, 2 for the second, etc.). Use with IF ERRORLEVEL to branch.

Note: CHOICE is an external command (not built into COMMAND.COM).

---

### REM

Add a comment (remark) to a batch file.

**Syntax:**

```
REM [comment text]
```

Everything on a REM line is ignored. In CONFIG.SYS/FDCONFIG.SYS, you can also use `;` for comments, but in batch files only `REM` works.

---

## External Commands

Utility programs found in C:\FREEDOS\BIN. These are separate executables.

### COMMAND

Start a new copy of the FreeDOS command shell (FreeCom).

**Syntax:**

```
COMMAND [[drive:]path] [/E:nnnnn] [/P] [/C command] [/K command]
```

**Key options:**

- `/E:nnnnn` -- Set environment size (256--32768 bytes).
- `/P` -- Make the shell permanent (cannot EXIT).
- `/C command` -- Execute command and return.
- `/K command` -- Execute command and keep running.
- `/MSG` -- Store error messages in memory (requires /P).
- `/LOW` -- Keep resident data in low memory.

In FDCONFIG.SYS, typically used as: `SHELL=C:\FREEDOS\BIN\COMMAND.COM C:\FREEDOS\BIN /E:1024 /P=C:\FDAUTO.BAT`

---

### EDIT

The FreeDOS text editor.

**Syntax:**

```
EDIT [/B] [/I] [/H] [/R] [file]
```

**Options:**

- `/B` -- Black and white (monochrome) display.
- `/I` -- Inverse color scheme.
- `/H` -- Use highest available text resolution (43/50 lines).
- `/R` -- Open files read-only.

EDIT has a built-in help system, supports mouse input, and includes features like a calendar and ASCII table. Cannot open files larger than 64 KB.

---

### MEM

Display the amount of installed and free memory.

**Syntax:**

```
MEM [/C] [/D] [/E] [/F] [/U] [/X] [/P]
```

**Key options:**

- `/C` -- Classify modules using memory below 1 MB.
- `/D` or `/DEBUG` -- Show programs and devices in conventional and upper memory.
- `/E` -- Report expanded memory (EMS) information.
- `/F` or `/FREE` -- Show free conventional and upper memory blocks.
- `/U` -- List programs in conventional and upper memory.
- `/X` -- Report extended memory (XMS) information.
- `/P` -- Pause after each screenful.

Useful for diagnosing memory issues when programs crash or refuse to load.

---

### MORE

Display output one screen at a time.

**Syntax:**

```
MORE [/Tn] file [file2 ...]
command | MORE
MORE < file
```

**Options:**

- `/Tn` -- Set tab size to n (1--9).

**Keys while viewing:**

- `Space` -- Next page.
- `N` -- Next file.
- `Q` -- Quit.

Supports wildcards and long filenames.

---

### DELTREE

Delete an entire directory tree including all files and subdirectories.

**Syntax:**

```
DELTREE [/Y] [/V] filespec [filespec ...]
```

**Options:**

- `/Y` -- Delete without prompting. Use with caution.
- `/V` -- Report counts and totals when finished.
- `/X` -- Test mode: show what would be deleted without actually deleting.
- `/D` -- Show debug information.

DELTREE ignores file attributes (read-only, hidden, system) and deletes everything.

---

### XCOPY

Copy files and directory trees.

**Syntax:**

```
XCOPY source [destination] [options]
```

**Key options:**

- `/S` -- Copy subdirectories (except empty ones).
- `/E` -- Copy subdirectories including empty ones.
- `/H` -- Copy hidden and system files.
- `/R` -- Overwrite read-only files.
- `/P` -- Prompt before creating each file.
- `/D[:date]` -- Copy only files changed on or after the specified date.
- `/A` -- Copy files with archive attribute set (don't clear it).
- `/M` -- Copy files with archive attribute set (clear it after copy).
- `/V` -- Verify each new file.
- `/Y` -- Suppress overwrite prompts.
- `/Q` -- Quiet mode.
- `/C` -- Continue copying even if errors occur.
- `/I` -- Assume destination is a directory when copying multiple files.

---

### FORMAT

Format a disk for use with FreeDOS.

**Syntax:**

```
FORMAT drive: [/V[:label]] [/Q] [/U] [/F:size] [/S]
```

**Key options:**

- `/V:label` -- Set volume label.
- `/Q` -- Quick format (preserves unformat data; this is the default for formatted disks).
- `/Q /U` -- Quick full format (fast, no recovery data).
- `/U` -- Unconditional format with surface scan (slow, destroys all data).
- `/F:size` -- Floppy size: 160, 180, 320, 360, 720, 1200, 1440, or 2880.
- `/S` -- Copy system files to make the disk bootable (requires SYS).
- `/A` -- Force 4K alignment for FAT32 metadata.

Supports FAT12, FAT16, and FAT32.

---

### FDISK

Create and manage hard disk partitions.

**Syntax:**

```
FDISK                              (interactive mode)
FDISK [drive#] /INFO               (display partition info)
FDISK [drive#] /PRI:size           (create primary partition)
FDISK [drive#] /EXT:size           (create extended partition)
FDISK [drive#] /LOG:size           (create logical drive)
FDISK [drive#] /ACTIVATE:part#     (set active partition)
FDISK [drive#] /DELETE /PRI[:n]    (delete primary partition)
FDISK [drive#] /AUTO               (auto-partition)
FDISK [drive#] /STATUS             (display partition layout)
FDISK [drive#] /IPL                (install standard boot code)
FDISK [drive#] /CLEARMBR           (delete all partitions and boot code)
```

FreeDOS supports up to four primary partitions. For more than four, create up to three primary partitions plus one extended partition containing logical drives. Only primary partitions can be booted.

After partitioning, use FORMAT to prepare partitions and SYS to make them bootable.

---

### LABEL

Create, change, or delete a disk volume label.

**Syntax:**

```
LABEL [drive:] [label]
```

The label can be up to 11 characters. If no label is specified, LABEL prompts for one. Network and SUBSTed drives cannot be labeled.

---

### ATTRIB

Display or change file attributes.

**Syntax:**

```
ATTRIB [+|-R] [+|-A] [+|-S] [+|-H] [/S] [/D] [drive:][path]filename
```

**Attributes:**

- `R` -- Read-only.
- `A` -- Archive (set when a file is modified).
- `S` -- System.
- `H` -- Hidden.

Use `+` to set and `-` to clear. `/S` processes files in subdirectories. `/D` processes directory names with wildcards.

---

### CHKDSK

Check a disk for errors.

**Syntax:**

```
CHKDSK [volume] [/F] [/R] [/S] [/V]
```

**Options:**

- `/F` -- Attempt to fix errors found.
- `/R` -- Scan the data area and try to recover unreadable data (slow).
- `/S` -- Show drive summary only.
- `/V` -- Show filenames as they are checked.

Note: CHKDSK supports FAT16 only. For FAT32, use DOSFSCK.

---

## Configuration (FDCONFIG.SYS)

FreeDOS reads FDCONFIG.SYS (or CONFIG.SYS if FDCONFIG.SYS does not exist) at startup to configure the system. It must be in the root directory of the boot drive. It is read before AUTOEXEC.BAT / FDAUTO.BAT.

Press **F5** during boot to skip CONFIG.SYS and AUTOEXEC.BAT entirely. Press **F8** to confirm each line individually.

### Common Directives

| Directive | Description |
|-----------|-------------|
| `DOS=HIGH,UMB` | Load DOS kernel into high memory area and enable upper memory blocks |
| `DEVICE=driver` | Load a device driver |
| `DEVICEHIGH=driver` | Load a device driver into upper memory |
| `FILES=n` | Set maximum number of open files (default 8, typical 40) |
| `BUFFERS=n` | Set number of disk buffers |
| `LASTDRIVE=Z` | Set the last available drive letter |
| `SHELL=path` | Specify the command shell to use |
| `SHELLHIGH=path` | Load the command shell into upper memory |
| `COUNTRY=code,codepage,file` | Set country-specific date/time/currency formats |
| `SET VAR=value` | Set an environment variable |
| `INSTALL=program` | Run a TSR program during boot |
| `DOSDATA=UMB` | Load DOS data tables into upper memory |
| `STACKS=n,size` | Set interrupt stack allocation |
| `NUMLOCK=ON\|OFF` | Set NumLock state at boot |

### Menu System

FDCONFIG.SYS supports boot menus for choosing between configurations:

```
MENUDEFAULT=1,5
MENU 1 - Load with JEMM386 (no EMS)
MENU 2 - Load with JEMM386 (EMS)
MENU 3 - Safe Mode
MENU 4 - Emergency Mode
```

Prefix lines with menu numbers to make them conditional: `12?DOS=HIGH` runs only for menu choices 1 and 2. Prefix with `!` to run for all menu choices: `!SET DOSDIR=C:\FREEDOS`.

### Typical Configuration

```
SET DOSDIR=C:\FREEDOS
LASTDRIVE=W
BUFFERS=20
FILES=40
DOS=HIGH
DOS=UMB
DOSDATA=UMB
DEVICE=C:\FREEDOS\BIN\himemx.exe
DEVICE=C:\FREEDOS\BIN\jemm386.exe NOEMS X=TEST I=TEST I=B000-B7FF
SHELLHIGH=C:\FREEDOS\BIN\command.com C:\FREEDOS\BIN /E:1024 /P=C:\FDAUTO.BAT
```

---

*Content adapted from the FreeDOS Help system (github.com/FDOS/help), originally by Jim Hall, Robert Platt, W. Spiegl, and the FreeDOS community. Licensed under the terms described in the FreeDOS Help H2Cpying file.*
