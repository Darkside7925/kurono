"""
Kurono OS — Linux Bridge Layer
Full GNU utilities integration simulated within the VFS.
"""

import os
import time
import random
from typing import List, Optional
from kurono_kernel import CommandEntry, ENV_LINUX


class LinuxBridge:
    """Simulates GNU/Linux commands operating on the Kurono VFS."""

    def __init__(self, kernel, vfs):
        self.kernel = kernel
        self.vfs = vfs
        self._register_commands()

    def _register_commands(self):
        cmds = [
            CommandEntry("ls",       ENV_LINUX, self.cmd_ls,       "List directory contents", "/bin/ls", "filesystem", ["dir"]),
            CommandEntry("cd",       ENV_LINUX, self.cmd_cd,       "Change directory", "/bin/cd", "filesystem"),
            CommandEntry("pwd",      ENV_LINUX, self.cmd_pwd,      "Print working directory", "/bin/pwd", "filesystem"),
            CommandEntry("cat",      ENV_LINUX, self.cmd_cat,      "Display file contents", "/bin/cat", "filesystem"),
            CommandEntry("mkdir",    ENV_LINUX, self.cmd_mkdir,    "Create directories", "/bin/mkdir", "filesystem"),
            CommandEntry("rmdir",    ENV_LINUX, self.cmd_rmdir,    "Remove empty directories", "/bin/rmdir", "filesystem"),
            CommandEntry("rm",       ENV_LINUX, self.cmd_rm,       "Remove files/directories", "/bin/rm", "filesystem"),
            CommandEntry("cp",       ENV_LINUX, self.cmd_cp,       "Copy files", "/bin/cp", "filesystem"),
            CommandEntry("mv",       ENV_LINUX, self.cmd_mv,       "Move/rename files", "/bin/mv", "filesystem"),
            CommandEntry("touch",    ENV_LINUX, self.cmd_touch,    "Create empty file or update timestamp", "/bin/touch", "filesystem"),
            CommandEntry("chmod",    ENV_LINUX, self.cmd_chmod,    "Change file permissions", "/bin/chmod", "filesystem"),
            CommandEntry("chown",    ENV_LINUX, self.cmd_chown,    "Change file ownership", "/bin/chown", "filesystem"),
            CommandEntry("find",     ENV_LINUX, self.cmd_find,     "Search for files", "/bin/find", "filesystem"),
            CommandEntry("grep",     ENV_LINUX, self.cmd_grep,     "Search file contents", "/bin/grep", "text"),
            CommandEntry("head",     ENV_LINUX, self.cmd_head,     "Show first lines of file", "/bin/head", "text"),
            CommandEntry("tail",     ENV_LINUX, self.cmd_tail,     "Show last lines of file", "/bin/tail", "text"),
            CommandEntry("wc",       ENV_LINUX, self.cmd_wc,       "Word/line/byte count", "/bin/wc", "text"),
            CommandEntry("echo",     ENV_LINUX, self.cmd_echo,     "Print text", "/bin/echo", "text"),
            CommandEntry("tee",      ENV_LINUX, self.cmd_tee,      "Read stdin and write to file", "/bin/tee", "text"),
            CommandEntry("sort",     ENV_LINUX, self.cmd_sort,     "Sort lines", "/bin/sort", "text"),
            CommandEntry("uniq",     ENV_LINUX, self.cmd_uniq,     "Remove duplicate lines", "/bin/uniq", "text"),
            CommandEntry("diff",     ENV_LINUX, self.cmd_diff,     "Compare files", "/bin/diff", "text"),
            CommandEntry("stat",     ENV_LINUX, self.cmd_stat,     "Display file status", "/bin/stat", "filesystem"),
            CommandEntry("du",       ENV_LINUX, self.cmd_du,       "Disk usage", "/bin/du", "filesystem"),
            CommandEntry("df",       ENV_LINUX, self.cmd_df,       "Disk free space", "/bin/df", "filesystem"),
            CommandEntry("ln",       ENV_LINUX, self.cmd_ln,       "Create links", "/bin/ln", "filesystem"),
            CommandEntry("tree",     ENV_LINUX, self.cmd_tree,     "Display directory tree", "/bin/tree", "filesystem"),
            CommandEntry("ps",       ENV_LINUX, self.cmd_ps,       "List processes", "/bin/ps", "system"),
            CommandEntry("kill",     ENV_LINUX, self.cmd_kill,     "Kill process", "/bin/kill", "system"),
            CommandEntry("top",      ENV_LINUX, self.cmd_top,      "Process monitor", "/bin/top", "system"),
            CommandEntry("whoami",   ENV_LINUX, self.cmd_whoami,   "Current username", "/bin/whoami", "system"),
            CommandEntry("hostname", ENV_LINUX, self.cmd_hostname, "Show hostname", "/bin/hostname", "system"),
            CommandEntry("uname",    ENV_LINUX, self.cmd_uname,    "System information", "/bin/uname", "system"),
            CommandEntry("date",     ENV_LINUX, self.cmd_date,     "Show date/time", "/bin/date", "system"),
            CommandEntry("uptime",   ENV_LINUX, self.cmd_uptime,   "Show uptime", "/bin/uptime", "system"),
            CommandEntry("clear",    ENV_LINUX, self.cmd_clear,    "Clear screen", "/bin/clear", "system"),
            CommandEntry("man",      ENV_LINUX, self.cmd_man,      "Manual pages", "/bin/man", "help"),
            CommandEntry("which",    ENV_LINUX, self.cmd_which,    "Show command path", "/bin/which", "help"),
            CommandEntry("history",  ENV_LINUX, self.cmd_history,  "Command history", "/bin/history", "help"),
            CommandEntry("export",   ENV_LINUX, self.cmd_export,   "Set environment variable", "/bin/export", "system"),
            CommandEntry("printenv", ENV_LINUX, self.cmd_printenv, "Print environment", "/bin/printenv", "system"),
            CommandEntry("id",       ENV_LINUX, self.cmd_id,       "User identity", "/bin/id", "system"),
            CommandEntry("ifconfig", ENV_LINUX, self.cmd_ifconfig, "Network interfaces", "/sbin/ifconfig", "network"),
            CommandEntry("ip",       ENV_LINUX, self.cmd_ip,       "IP configuration", "/sbin/ip", "network"),
            CommandEntry("ping",     ENV_LINUX, self.cmd_ping,     "Ping host", "/bin/ping", "network"),
            CommandEntry("netstat",  ENV_LINUX, self.cmd_netstat,  "Network statistics", "/bin/netstat", "network"),
            CommandEntry("curl",     ENV_LINUX, self.cmd_curl,     "Transfer URL", "/bin/curl", "network"),
            CommandEntry("wget",     ENV_LINUX, self.cmd_wget,     "Download files", "/bin/wget", "network"),
            CommandEntry("ssh",      ENV_LINUX, self.cmd_ssh,      "Secure shell", "/bin/ssh", "network"),
            CommandEntry("tar",      ENV_LINUX, self.cmd_tar,      "Archive files", "/bin/tar", "archive"),
            CommandEntry("gzip",     ENV_LINUX, self.cmd_gzip,     "Compress files", "/bin/gzip", "archive"),
            CommandEntry("zip",      ENV_LINUX, self.cmd_zip,      "Create zip archive", "/bin/zip", "archive"),
            CommandEntry("apt",      ENV_LINUX, self.cmd_apt,      "Package manager (Debian)", "/bin/apt", "package"),
            CommandEntry("systemctl",ENV_LINUX, self.cmd_systemctl,"Service manager", "/bin/systemctl", "system"),
            CommandEntry("mount",    ENV_LINUX, self.cmd_mount,    "Mount filesystem", "/bin/mount", "filesystem"),
            CommandEntry("umount",   ENV_LINUX, self.cmd_umount,   "Unmount filesystem", "/bin/umount", "filesystem"),
            CommandEntry("free",     ENV_LINUX, self.cmd_free,     "Memory usage", "/bin/free", "system"),
            CommandEntry("lsblk",    ENV_LINUX, self.cmd_lsblk,   "List block devices", "/bin/lsblk", "system"),
            CommandEntry("dmesg",    ENV_LINUX, self.cmd_dmesg,    "Kernel messages", "/bin/dmesg", "system"),
        ]
        self.kernel.register_many(cmds)

    # ── Helpers ───────────────────────────────────────────────────────────────

    def _parse_args(self, args: List[str]):
        flags = set()
        positional = []
        for a in args:
            if a.startswith("-"):
                for c in a.lstrip("-"):
                    flags.add(c)
            else:
                positional.append(a)
        return flags, positional

    def _format_time(self, t: float) -> str:
        return time.strftime("%Y-%m-%d %H:%M", time.localtime(t))

    def _format_perms(self, node):
        prefix = "d" if node.is_dir() else "-"
        return prefix + node.permissions

    # ── Filesystem commands ───────────────────────────────────────────────────

    def cmd_ls(self, args: List[str], stdin: str = "") -> str:
        flags, paths = self._parse_args(args)
        path = paths[0] if paths else None
        try:
            nodes = self.vfs.listdir_nodes(path)
        except Exception as e:
            return f"ls: {e}"
        if not nodes:
            return ""
        if "l" in flags:
            lines = []
            for n in nodes:
                sz = n.size()
                ts = self._format_time(n.modified)
                perms = self._format_perms(n)
                name = n.name + ("/" if n.is_dir() else "")
                lines.append(f"{perms}  {n.owner:8s} {n.group:8s} {sz:>8d}  {ts}  {name}")
            if "a" in flags:
                lines.insert(0, f"drwxr-xr-x  .         .               0  {self._format_time(time.time())}  .")
                lines.insert(1, f"drwxr-xr-x  .         .               0  {self._format_time(time.time())}  ..")
            return "\n".join(lines)
        else:
            names = []
            for n in nodes:
                name = n.name + ("/" if n.is_dir() else "")
                if n.name.startswith(".") and "a" not in flags:
                    continue
                names.append(name)
            return "  ".join(names)

    def cmd_cd(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        path = paths[0] if paths else "~"
        try:
            self.vfs.chdir(path)
            self.kernel.set_var("PWD", self.vfs.getcwd())
            return ""
        except Exception as e:
            return f"cd: {e}"

    def cmd_pwd(self, args: List[str], stdin: str = "") -> str:
        return self.vfs.getcwd()

    def cmd_cat(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if not paths:
            return stdin or ""
        output = []
        for p in paths:
            try:
                output.append(self.vfs.read_file(p))
            except Exception as e:
                output.append(f"cat: {e}")
        return "\n".join(output)

    def cmd_mkdir(self, args: List[str], stdin: str = "") -> str:
        flags, paths = self._parse_args(args)
        for p in paths:
            try:
                if "p" in flags:
                    self.vfs.makedirs(p, exist_ok=True)
                else:
                    self.vfs.mkdir(p)
            except Exception as e:
                return f"mkdir: {e}"
        return ""

    def cmd_rmdir(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        for p in paths:
            try:
                self.vfs.remove(p)
            except Exception as e:
                return f"rmdir: {e}"
        return ""

    def cmd_rm(self, args: List[str], stdin: str = "") -> str:
        flags, paths = self._parse_args(args)
        for p in paths:
            try:
                if "r" in flags or "R" in flags:
                    self.vfs.rmtree(p)
                else:
                    self.vfs.remove(p)
            except Exception as e:
                return f"rm: {e}"
        return ""

    def cmd_cp(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if len(paths) < 2:
            return "cp: missing operand"
        try:
            self.vfs.copy(paths[0], paths[1])
        except Exception as e:
            return f"cp: {e}"
        return ""

    def cmd_mv(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if len(paths) < 2:
            return "mv: missing operand"
        try:
            self.vfs.move(paths[0], paths[1])
        except Exception as e:
            return f"mv: {e}"
        return ""

    def cmd_touch(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        for p in paths:
            if not self.vfs.exists(p):
                try:
                    self.vfs.write_file(p, "", create_parents=True)
                except Exception as e:
                    return f"touch: {e}"
        return ""

    def cmd_chmod(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if len(paths) < 2:
            return "chmod: usage: chmod PERMS FILE"
        try:
            self.vfs.chmod(paths[1], paths[0])
        except Exception as e:
            return f"chmod: {e}"
        return ""

    def cmd_chown(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if len(paths) < 2:
            return "chown: usage: chown OWNER FILE"
        owner = paths[0]
        group = None
        if ":" in owner:
            owner, group = owner.split(":", 1)
        try:
            self.vfs.chown(paths[1], owner, group)
        except Exception as e:
            return f"chown: {e}"
        return ""

    def cmd_find(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        root = paths[0] if paths else "."
        name = None
        if "-name" in args:
            idx = args.index("-name")
            if idx + 1 < len(args):
                name = args[idx + 1]
        try:
            results = self.vfs.find(root, name)
            return "\n".join(results)
        except Exception as e:
            return f"find: {e}"

    def cmd_grep(self, args: List[str], stdin: str = "") -> str:
        flags, positional = self._parse_args(args)
        if not positional:
            return "grep: missing pattern"
        pattern = positional[0]
        files = positional[1:] if len(positional) > 1 else []
        recursive = "r" in flags or "R" in flags

        if stdin and not files:
            import re
            results = []
            for i, line in enumerate(stdin.splitlines(), 1):
                if re.search(pattern, line, re.IGNORECASE):
                    results.append(line)
            return "\n".join(results)

        output = []
        for f in files:
            try:
                results = self.vfs.grep(pattern, f, recursive)
                for fpath, lnum, line in results:
                    output.append(f"{fpath}:{lnum}:{line}")
            except Exception as e:
                output.append(f"grep: {e}")
        return "\n".join(output)

    def cmd_head(self, args: List[str], stdin: str = "") -> str:
        flags, paths = self._parse_args(args)
        n = 10
        if "n" in flags and paths:
            try:
                n = int(paths.pop(0))
            except ValueError:
                pass
        text = stdin if not paths else ""
        if paths:
            try:
                text = self.vfs.read_file(paths[0])
            except Exception as e:
                return f"head: {e}"
        return "\n".join(text.splitlines()[:n])

    def cmd_tail(self, args: List[str], stdin: str = "") -> str:
        flags, paths = self._parse_args(args)
        n = 10
        text = stdin if not paths else ""
        if paths:
            try:
                text = self.vfs.read_file(paths[0])
            except Exception as e:
                return f"tail: {e}"
        return "\n".join(text.splitlines()[-n:])

    def cmd_wc(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        text = stdin
        if paths:
            try:
                text = self.vfs.read_file(paths[0])
            except Exception as e:
                return f"wc: {e}"
        lines = len(text.splitlines())
        words = len(text.split())
        chars = len(text)
        name = paths[0] if paths else ""
        return f"  {lines:6d}  {words:6d}  {chars:6d} {name}"

    def cmd_echo(self, args: List[str], stdin: str = "") -> str:
        text = " ".join(args)
        text = self.kernel.expand_vars(text)
        # Check for redirect
        if ">" in text:
            parts = text.split(">", 1)
            content = parts[0].strip()
            target = parts[1].strip()
            try:
                self.vfs.write_file(target, content + "\n", create_parents=True)
            except Exception as e:
                return f"echo: {e}"
            return ""
        if ">>" in text:
            parts = text.split(">>", 1)
            content = parts[0].strip()
            target = parts[1].strip()
            try:
                self.vfs.write_file(target, content + "\n", create_parents=True, append=True)
            except Exception as e:
                return f"echo: {e}"
            return ""
        return text

    def cmd_tee(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        for p in paths:
            try:
                self.vfs.write_file(p, stdin, create_parents=True)
            except Exception:
                pass
        return stdin

    def cmd_sort(self, args: List[str], stdin: str = "") -> str:
        text = stdin
        _, paths = self._parse_args(args)
        if paths:
            try:
                text = self.vfs.read_file(paths[0])
            except Exception as e:
                return f"sort: {e}"
        return "\n".join(sorted(text.splitlines()))

    def cmd_uniq(self, args: List[str], stdin: str = "") -> str:
        lines = stdin.splitlines()
        result = []
        prev = None
        for line in lines:
            if line != prev:
                result.append(line)
                prev = line
        return "\n".join(result)

    def cmd_diff(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if len(paths) < 2:
            return "diff: need two files"
        try:
            a = self.vfs.read_file(paths[0]).splitlines()
            b = self.vfs.read_file(paths[1]).splitlines()
        except Exception as e:
            return f"diff: {e}"
        output = []
        for i, (la, lb) in enumerate(zip(a, b), 1):
            if la != lb:
                output.append(f"{i}c{i}")
                output.append(f"< {la}")
                output.append("---")
                output.append(f"> {lb}")
        if len(a) != len(b):
            output.append(f"Files differ in length: {len(a)} vs {len(b)} lines")
        if not output:
            return ""
        return "\n".join(output)

    def cmd_stat(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        if not paths:
            return "stat: missing file"
        try:
            s = self.vfs.stat(paths[0])
            return (f"  File: {s['name']}\n"
                    f"  Size: {s['size']}\tType: {s['type']}\n"
                    f" Owner: {s['owner']}\tGroup: {s['group']}\n"
                    f" Perms: {s['permissions']}\n"
                    f"Modify: {self._format_time(s['modified'])}\n"
                    f"Access: {self._format_time(s['accessed'])}\n"
                    f" Birth: {self._format_time(s['created'])}")
        except Exception as e:
            return f"stat: {e}"

    def cmd_du(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        path = paths[0] if paths else "."
        try:
            sz = self.vfs.du(path)
            return f"{sz}\t{path}"
        except Exception as e:
            return f"du: {e}"

    def cmd_df(self, args: List[str], stdin: str = "") -> str:
        d = self.vfs.df()
        return (f"Filesystem      Size    Used    Avail   Use%\n"
                f"/dev/kurono0    {self.vfs.format_size(d['total']):>7s} "
                f"{self.vfs.format_size(d['used']):>7s} "
                f"{self.vfs.format_size(d['free']):>7s} "
                f"{d['percent']:>5.1f}%")

    def cmd_ln(self, args: List[str], stdin: str = "") -> str:
        return "ln: symbolic links not yet implemented"

    def cmd_tree(self, args: List[str], stdin: str = "") -> str:
        _, paths = self._parse_args(args)
        path = paths[0] if paths else "."
        lines = []
        self._tree_recursive(path, "", lines, max_depth=3, depth=0)
        return "\n".join(lines) if lines else "(empty)"

    def _tree_recursive(self, path: str, prefix: str, lines: list, max_depth: int, depth: int):
        if depth > max_depth:
            return
        try:
            nodes = self.vfs.listdir_nodes(path)
        except Exception:
            return
        for i, node in enumerate(nodes):
            is_last = (i == len(nodes) - 1)
            connector = "└── " if is_last else "├── "
            suffix = "/" if node.is_dir() else ""
            lines.append(f"{prefix}{connector}{node.name}{suffix}")
            if node.is_dir():
                ext = "    " if is_last else "│   "
                child_path = path.rstrip("/") + "/" + node.name
                self._tree_recursive(child_path, prefix + ext, lines, max_depth, depth + 1)

    # ── System commands ───────────────────────────────────────────────────────

    def cmd_ps(self, args: List[str], stdin: str = "") -> str:
        procs = self.kernel.list_processes()
        lines = [f"{'PID':>6s}  {'USER':8s}  {'ENV':8s}  {'%CPU':>5s}  {'%MEM':>5s}  {'STAT':6s}  {'TIME':>8s}  COMMAND"]
        for p in procs:
            lines.append(f"{p.pid:6d}  {p.user:8s}  {p.env:8s}  {p.cpu:5.1f}  {p.mem:5.1f}  {p.status:6s}  {p.runtime():>8s}  {p.name}")
        return "\n".join(lines)

    def cmd_kill(self, args: List[str], stdin: str = "") -> str:
        _, pids = self._parse_args(args)
        for p in pids:
            try:
                pid = int(p)
                if self.kernel.kill_process(pid):
                    return f"Process {pid} killed"
                return f"kill: no such process {pid}"
            except ValueError:
                return f"kill: invalid PID: {p}"
        return ""

    def cmd_top(self, args: List[str], stdin: str = "") -> str:
        info = self.kernel.sysinfo()
        header = (f"top - {time.strftime('%H:%M:%S')} up {info['uptime']}, "
                  f"{info['processes']} processes\n"
                  f"Tasks: {info['processes']} total, {len([p for p in self.kernel.processes.values() if p.status=='running'])} running\n"
                  f"Cpu(s): {random.uniform(1,15):.1f}% us, {random.uniform(0,5):.1f}% sy, {random.uniform(0,2):.1f}% ni\n"
                  f"MiB Mem: 8192.0 total, {random.randint(2000,5000):.0f} free, {random.randint(1500,4000):.0f} used\n")
        return header + "\n" + self.cmd_ps(["aux"])

    def cmd_whoami(self, args: List[str], stdin: str = "") -> str:
        return self.kernel.get_var("USER", "user")

    def cmd_hostname(self, args: List[str], stdin: str = "") -> str:
        return self.kernel.get_var("HOSTNAME", "kurono-machine")

    def cmd_uname(self, args: List[str], stdin: str = "") -> str:
        flags, _ = self._parse_args(args)
        if "a" in flags:
            return f"Kurono {self.kernel.VERSION} kurono-machine x86_64 KuronoOS"
        return "Kurono"

    def cmd_date(self, args: List[str], stdin: str = "") -> str:
        return time.strftime("%a %b %d %H:%M:%S %Z %Y")

    def cmd_uptime(self, args: List[str], stdin: str = "") -> str:
        return f" {time.strftime('%H:%M:%S')} up {self.kernel.uptime()}, 1 user, load average: {random.uniform(0,2):.2f}, {random.uniform(0,2):.2f}, {random.uniform(0,2):.2f}"

    def cmd_clear(self, args: List[str], stdin: str = "") -> str:
        return "\x1b[CLEAR]"  # signal for GUI terminal to clear

    def cmd_man(self, args: List[str], stdin: str = "") -> str:
        _, cmds = self._parse_args(args)
        if not cmds:
            return "What manual page do you want?\nUsage: man <command>"
        cmd = cmds[0]
        entries = self.kernel.lookup(cmd)
        if not entries:
            return f"No manual entry for {cmd}"
        e = entries[0]
        return (f"NAME\n    {e.name} - {e.description}\n\n"
                f"SYNOPSIS\n    {e.name} [options] [args...]\n\n"
                f"ENVIRONMENT\n    {e.env}\n\n"
                f"PATH\n    {e.path}\n\n"
                f"CATEGORY\n    {e.category}")

    def cmd_which(self, args: List[str], stdin: str = "") -> str:
        _, cmds = self._parse_args(args)
        results = []
        for cmd in cmds:
            entries = self.kernel.lookup(cmd)
            if entries:
                results.append(entries[0].path)
            else:
                results.append(f"{cmd}: not found")
        return "\n".join(results)

    def cmd_history(self, args: List[str], stdin: str = "") -> str:
        lines = []
        for i, h in enumerate(self.kernel.history[-50:], 1):
            lines.append(f"  {i:4d}  {h}")
        return "\n".join(lines)

    def cmd_export(self, args: List[str], stdin: str = "") -> str:
        for a in args:
            if "=" in a:
                k, v = a.split("=", 1)
                self.kernel.set_var(k, v)
        return ""

    def cmd_printenv(self, args: List[str], stdin: str = "") -> str:
        if args:
            return self.kernel.get_var(args[0], "")
        return "\n".join(f"{k}={v}" for k, v in sorted(self.kernel.env_variables.items()))

    def cmd_id(self, args: List[str], stdin: str = "") -> str:
        user = self.kernel.get_var("USER", "user")
        uid = 0 if user == "root" else 1000
        return f"uid={uid}({user}) gid={uid}({user}) groups={uid}({user})"

    # ── Network commands ──────────────────────────────────────────────────────

    def cmd_ifconfig(self, args: List[str], stdin: str = "") -> str:
        return (
            "eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500\n"
            "        inet 192.168.1.100  netmask 255.255.255.0  broadcast 192.168.1.255\n"
            "        inet6 fe80::1  prefixlen 64  scopeid 0x20<link>\n"
            "        RX packets 124523  bytes 98234521 (93.7 MiB)\n"
            "        TX packets 85432  bytes 12345678 (11.7 MiB)\n\n"
            "wlan0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500\n"
            "        inet 192.168.1.101  netmask 255.255.255.0  broadcast 192.168.1.255\n"
            "        RX packets 54321  bytes 45678901 (43.5 MiB)\n"
            "        TX packets 32109  bytes 9876543 (9.4 MiB)\n\n"
            "lo: flags=73<UP,LOOPBACK,RUNNING>  mtu 65536\n"
            "        inet 127.0.0.1  netmask 255.0.0.0\n"
            "        loop  txqueuelen 1000 (Local Loopback)"
        )

    def cmd_ip(self, args: List[str], stdin: str = "") -> str:
        return (
            "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536\n"
            "    inet 127.0.0.1/8 scope host lo\n"
            "2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500\n"
            "    inet 192.168.1.100/24 brd 192.168.1.255 scope global eth0\n"
            "3: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500\n"
            "    inet 192.168.1.101/24 brd 192.168.1.255 scope global wlan0"
        )

    def cmd_ping(self, args: List[str], stdin: str = "") -> str:
        _, hosts = self._parse_args(args)
        host = hosts[0] if hosts else "localhost"
        lines = [f"PING {host} (127.0.0.1) 56(84) bytes of data."]
        for i in range(4):
            t = round(random.uniform(0.5, 15.0), 2)
            lines.append(f"64 bytes from {host}: icmp_seq={i+1} ttl=64 time={t} ms")
        lines.append(f"\n--- {host} ping statistics ---")
        lines.append("4 packets transmitted, 4 received, 0% packet loss")
        return "\n".join(lines)

    def cmd_netstat(self, args: List[str], stdin: str = "") -> str:
        return (
            "Proto Recv-Q Send-Q Local Address           Foreign Address         State\n"
            "tcp        0      0 0.0.0.0:22              0.0.0.0:*               LISTEN\n"
            "tcp        0      0 0.0.0.0:80              0.0.0.0:*               LISTEN\n"
            "tcp        0      0 192.168.1.100:443       0.0.0.0:*               LISTEN\n"
            "tcp        0      0 192.168.1.100:52341     93.184.216.34:443       ESTABLISHED\n"
            "udp        0      0 0.0.0.0:68              0.0.0.0:*                \n"
            "udp        0      0 0.0.0.0:5353            0.0.0.0:*                "
        )

    def cmd_curl(self, args: List[str], stdin: str = "") -> str:
        _, urls = self._parse_args(args)
        if not urls:
            return "curl: no URL specified"
        return f"<!DOCTYPE html>\n<html><body><h1>Kurono OS Web</h1><p>Response from {urls[0]}</p></body></html>"

    def cmd_wget(self, args: List[str], stdin: str = "") -> str:
        _, urls = self._parse_args(args)
        if not urls:
            return "wget: no URL specified"
        return f"--{time.strftime('%Y-%m-%d %H:%M:%S')}--  {urls[0]}\nResolving... done.\nConnecting... connected.\nHTTP request sent, awaiting response... 200 OK\nLength: 1024 (1.0K) [text/html]\nSaving to: 'index.html'\nindex.html          100%[===================>]   1.00K  --.-KB/s    in 0s"

    def cmd_ssh(self, args: List[str], stdin: str = "") -> str:
        return "ssh: connection simulated (Kurono secure shell)"

    # ── Archive ───────────────────────────────────────────────────────────────

    def cmd_tar(self, args: List[str], stdin: str = "") -> str:
        return "tar: archive operation simulated"

    def cmd_gzip(self, args: List[str], stdin: str = "") -> str:
        return "gzip: compression simulated"

    def cmd_zip(self, args: List[str], stdin: str = "") -> str:
        return "zip: archive created (simulated)"

    # ── Package / Service ─────────────────────────────────────────────────────

    def cmd_apt(self, args: List[str], stdin: str = "") -> str:
        return "apt: use Kurono 'install'/'remove' commands for package management"

    def cmd_systemctl(self, args: List[str], stdin: str = "") -> str:
        _, positional = self._parse_args(args)
        if not positional:
            return "systemctl: usage: systemctl [start|stop|status|restart] <service>"
        action = positional[0]
        service = positional[1] if len(positional) > 1 else "unknown"
        return f"● {service}.service - {service} daemon\n   Loaded: loaded (/etc/kurono/system/{service}.service)\n   Active: active (running)\n   PID: {random.randint(1000,9999)}"

    def cmd_mount(self, args: List[str], stdin: str = "") -> str:
        return ("/dev/kurono0 on / type kfs (rw,relatime)\n"
                "proc on /proc type proc (rw,nosuid,nodev,noexec)\n"
                "sysfs on /sys type sysfs (rw,nosuid,nodev,noexec)\n"
                "tmpfs on /tmp type tmpfs (rw,nosuid,nodev)")

    def cmd_umount(self, args: List[str], stdin: str = "") -> str:
        return "umount: filesystem unmounted (simulated)"

    def cmd_free(self, args: List[str], stdin: str = "") -> str:
        total = 8192
        used = random.randint(2500, 5000)
        free = total - used
        return (f"              total        used        free      shared  buff/cache   available\n"
                f"Mem:          {total:5d}       {used:5d}       {free:5d}         128        2048        {free+1024:5d}\n"
                f"Swap:          4096           0        4096")

    def cmd_lsblk(self, args: List[str], stdin: str = "") -> str:
        return (
            "NAME          MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS\n"
            "kda             8:0    0    64G  0 disk\n"
            "├─kda1          8:1    0   512M  0 part /boot\n"
            "├─kda2          8:2    0    60G  0 part /\n"
            "└─kda3          8:3    0   3.5G  0 part [SWAP]\n"
            "kdb             8:16   0   256G  0 disk\n"
            "└─kdb1          8:17   0   256G  0 part /home"
        )

    def cmd_dmesg(self, args: List[str], stdin: str = "") -> str:
        msgs = [
            "[    0.000000] Kurono Kernel 1.0.0 booting...",
            "[    0.001234] Memory: 8388608K/8388608K available",
            "[    0.005000] CPU: Kurono Processor @ 3.60GHz",
            "[    0.010000] PCI: Probing PCI hardware",
            "[    0.050000] kurono_vfs: Virtual filesystem initialized",
            "[    0.100000] kurono_net: Network stack initialized",
            "[    0.150000] kurono_wifi: WiFi driver kurono_wifi_ng loaded",
            "[    0.200000] kurono_audio: Audio subsystem initialized",
            "[    0.250000] kurono_gpu: Display driver loaded (1920x1080)",
            "[    0.300000] kurono_usb: USB hub detected",
            "[    0.350000] linux_bridge: Linux subsystem ready",
            "[    0.400000] windows_bridge: Windows subsystem ready",
            "[    0.450000] supr_engine: Security engine initialized",
            "[    0.500000] kurono_gui: Desktop environment starting...",
            f"[    0.600000] Kurono OS {self.kernel.VERSION} ready.",
        ]
        return "\n".join(msgs)
