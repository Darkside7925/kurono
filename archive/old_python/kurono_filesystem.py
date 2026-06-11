"""
Kurono OS — Virtual Filesystem (VFS)
Implements a complete in-memory + JSON-persisted filesystem with POSIX + Windows semantics.
"""

import os
import json
import time
import hashlib
import shutil
from pathlib import Path
from typing import Optional, Dict, List, Any, Tuple

VFS_DATA_PATH = os.path.join(os.path.dirname(__file__), "kurono_vfs_data.json")

# ─── Node Types ───────────────────────────────────────────────────────────────

class VFSNode:
    def __init__(self, name: str, node_type: str, owner: str = "root",
                 permissions: str = "rwxr-xr-x", parent=None):
        self.name = name
        self.node_type = node_type          # "file" | "dir" | "symlink" | "device"
        self.owner = owner
        self.group = "kurono"
        self.permissions = permissions
        self.created = time.time()
        self.modified = time.time()
        self.accessed = time.time()
        self.parent: Optional['VFSNode'] = parent
        self.children: Dict[str, 'VFSNode'] = {}
        self.content: str = ""
        self.symlink_target: str = ""
        self.metadata: Dict[str, Any] = {}

    def is_dir(self) -> bool:
        return self.node_type == "dir"

    def is_file(self) -> bool:
        return self.node_type == "file"

    def size(self) -> int:
        if self.is_file():
            return len(self.content.encode())
        return sum(c.size() for c in self.children.values())

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "node_type": self.node_type,
            "owner": self.owner,
            "group": self.group,
            "permissions": self.permissions,
            "created": self.created,
            "modified": self.modified,
            "accessed": self.accessed,
            "content": self.content,
            "symlink_target": self.symlink_target,
            "metadata": self.metadata,
            "children": {k: v.to_dict() for k, v in self.children.items()},
        }

    @staticmethod
    def from_dict(data: dict, parent=None) -> 'VFSNode':
        node = VFSNode(data["name"], data["node_type"], data.get("owner", "root"), data.get("permissions", "rwxr-xr-x"), parent)
        node.group = data.get("group", "kurono")
        node.created = data.get("created", time.time())
        node.modified = data.get("modified", time.time())
        node.accessed = data.get("accessed", time.time())
        node.content = data.get("content", "")
        node.symlink_target = data.get("symlink_target", "")
        node.metadata = data.get("metadata", {})
        for k, v in data.get("children", {}).items():
            child = VFSNode.from_dict(v, parent=node)
            node.children[k] = child
        return node


# ─── VFS Core ─────────────────────────────────────────────────────────────────

class KuronoVFS:
    """Full virtual filesystem with POSIX semantics and persistence."""

    def __init__(self):
        self.root = VFSNode("/", "dir", "root", "rwxr-xr-x")
        self.cwd_path = "/"
        self._build_default_tree()

    # ── Tree builder ──────────────────────────────────────────────────────────

    def _build_default_tree(self):
        dirs = [
            "/bin", "/sbin", "/usr", "/usr/bin", "/usr/lib", "/usr/share",
            "/etc", "/etc/kurono", "/etc/network", "/etc/wifi",
            "/home", "/home/user", "/home/user/Documents", "/home/user/Downloads",
            "/home/user/Desktop", "/home/user/Pictures", "/home/user/Music",
            "/home/user/Videos",
            "/var", "/var/log", "/var/lib", "/var/cache",
            "/tmp", "/proc", "/sys", "/dev",
            "/lib", "/lib64",
            "/root", "/root/Documents",
            "/opt", "/opt/kurono",
            "/mnt", "/mnt/windows", "/mnt/usb",
            # Windows bridge paths
            "/windows", "/windows/System32", "/windows/Users",
            "/windows/Users/Default", "/windows/Program Files",
            "/windows/Program Files (x86)",
            # Kurono native
            "/kurono", "/kurono/apps", "/kurono/packages", "/kurono/scripts",
            "/kurono/themes", "/kurono/drivers", "/kurono/drivers/wifi",
            "/kurono/drivers/audio", "/kurono/drivers/video",
            "/kurono/logs", "/kurono/config",
        ]
        for d in dirs:
            self.makedirs(d, exist_ok=True)

        # Default files
        default_files = {
            "/etc/kurono/os-release": "NAME=\"Kurono OS\"\nVERSION=\"1.0.0\"\nID=kurono\nPRETTY_NAME=\"Kurono OS 1.0.0\"\nHOME_URL=\"https://kurono.os\"\n",
            "/etc/kurono/hostname": "kurono-machine\n",
            "/etc/kurono/passwd": "root:x:0:0:root:/root:/bin/ksh\nuser:x:1000:1000:Kurono User:/home/user:/bin/ksh\n",
            "/etc/kurono/version": "1.0.0",
            "/etc/network/interfaces": "auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n",
            "/etc/wifi/wpa_supplicant.conf": "ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"KuronoNet\"\n  psk=\"password\"\n}\n",
            "/home/user/Documents/readme.txt": "Welcome to Kurono OS!\n\nThis is your Documents folder. You can store files here.\nUse the Kurono Shell to navigate and manage files.\n\nType 'help' to get started.\n",
            "/home/user/Desktop/welcome.kcl": 'set name = "User"\nprint "Welcome to Kurono OS, $name!"\nprint "Type help to see available commands."\n',
            "/kurono/config/theme.json": '{"name": "Kurono Dark", "accent": "#6C63FF", "bg": "#0D0D1A", "glass": true}',
            "/var/log/kurono.log": f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] Kurono OS booted successfully.\n",
            "/proc/version": "Kurono 1.0.0 (kurono@kurono-machine) (kurono-gcc 1.0) #1 SMP",
            "/proc/cpuinfo": "processor\t: 0\nvendor_id\t: KuronoChip\nmodel name\t: Kurono Processor @ 3.60GHz\ncpu cores\t: 4\n",
            "/proc/meminfo": "MemTotal:        8388608 kB\nMemFree:         4194304 kB\nMemAvailable:    5242880 kB\n",
            "/bin/ksh": "#!/kurono/bin/ksh\n# Kurono Shell binary\n",
            "/kurono/drivers/wifi/driver.ini": "[wifi]\ndriver=kurono_wifi_ng\nversion=1.0\nsupported_chips=Intel,Broadcom,Realtek,Atheros\n",
        }
        for path, content in default_files.items():
            try:
                self.write_file(path, content, create_parents=True)
            except Exception:
                pass

    # ── Path resolution ───────────────────────────────────────────────────────

    def resolve(self, path: str) -> str:
        """Resolve a path to absolute."""
        if not path:
            return self.cwd_path
        if path == "~":
            return "/home/user"
        if path.startswith("~"):
            return "/home/user" + path[1:]
        if not path.startswith("/"):
            path = (self.cwd_path.rstrip("/") + "/" + path)
        # Normalize
        parts = []
        for p in path.split("/"):
            if p == "" or p == ".":
                continue
            elif p == "..":
                if parts:
                    parts.pop()
            else:
                parts.append(p)
        return "/" + "/".join(parts)

    def _get_node(self, path: str) -> Optional[VFSNode]:
        path = self.resolve(path)
        if path == "/":
            return self.root
        node = self.root
        for part in path.strip("/").split("/"):
            if part not in node.children:
                return None
            node = node.children[part]
        return node

    def _get_parent_and_name(self, path: str) -> Tuple[Optional[VFSNode], str]:
        path = self.resolve(path)
        if path == "/":
            return None, "/"
        parts = path.strip("/").split("/")
        name = parts[-1]
        parent_path = "/" + "/".join(parts[:-1])
        parent = self._get_node(parent_path)
        return parent, name

    # ── Core operations ───────────────────────────────────────────────────────

    def exists(self, path: str) -> bool:
        return self._get_node(path) is not None

    def is_dir(self, path: str) -> bool:
        n = self._get_node(path)
        return n is not None and n.is_dir()

    def is_file(self, path: str) -> bool:
        n = self._get_node(path)
        return n is not None and n.is_file()

    def makedirs(self, path: str, exist_ok: bool = False):
        path = self.resolve(path)
        parts = [p for p in path.strip("/").split("/") if p]
        node = self.root
        current = "/"
        for part in parts:
            current = current.rstrip("/") + "/" + part
            if part not in node.children:
                new_node = VFSNode(part, "dir", parent=node)
                node.children[part] = new_node
            elif not node.children[part].is_dir():
                if not exist_ok:
                    raise FileExistsError(f"Not a directory: {current}")
            node = node.children[part]

    def mkdir(self, path: str, exist_ok: bool = False):
        path = self.resolve(path)
        parent, name = self._get_parent_and_name(path)
        if parent is None:
            raise PermissionError("Cannot create root")
        if name in parent.children:
            if not exist_ok:
                raise FileExistsError(f"Already exists: {path}")
            return
        parent.children[name] = VFSNode(name, "dir", parent=parent)
        parent.modified = time.time()

    def write_file(self, path: str, content: str, create_parents: bool = False, append: bool = False):
        path = self.resolve(path)
        parent, name = self._get_parent_and_name(path)
        if parent is None:
            raise PermissionError("Cannot write to root")
        if create_parents and parent is None:
            self.makedirs(str(Path(path).parent), exist_ok=True)
            parent, name = self._get_parent_and_name(path)
        if name in parent.children and parent.children[name].is_dir():
            raise IsADirectoryError(f"Is a directory: {path}")
        if name not in parent.children:
            node = VFSNode(name, "file", parent=parent)
            parent.children[name] = node
        else:
            node = parent.children[name]
        if append:
            node.content += content
        else:
            node.content = content
        node.modified = time.time()
        parent.modified = time.time()

    def read_file(self, path: str) -> str:
        node = self._get_node(path)
        if node is None:
            raise FileNotFoundError(f"No such file: {path}")
        if node.is_dir():
            raise IsADirectoryError(f"Is a directory: {path}")
        node.accessed = time.time()
        return node.content

    def listdir(self, path: str = None) -> List[str]:
        if path is None:
            path = self.cwd_path
        node = self._get_node(path)
        if node is None:
            raise FileNotFoundError(f"No such directory: {path}")
        if not node.is_dir():
            raise NotADirectoryError(f"Not a directory: {path}")
        return sorted(node.children.keys())

    def listdir_nodes(self, path: str = None) -> List[VFSNode]:
        if path is None:
            path = self.cwd_path
        node = self._get_node(path)
        if node is None:
            raise FileNotFoundError(f"No such directory: {path}")
        return sorted(node.children.values(), key=lambda n: (not n.is_dir(), n.name))

    def remove(self, path: str):
        parent, name = self._get_parent_and_name(path)
        if parent is None or name not in parent.children:
            raise FileNotFoundError(f"No such file: {path}")
        node = parent.children[name]
        if node.is_dir() and node.children:
            raise OSError(f"Directory not empty: {path}")
        del parent.children[name]
        parent.modified = time.time()

    def rmtree(self, path: str):
        parent, name = self._get_parent_and_name(path)
        if parent is None or name not in parent.children:
            raise FileNotFoundError(f"No such path: {path}")
        del parent.children[name]
        parent.modified = time.time()

    def copy(self, src: str, dst: str):
        src_node = self._get_node(src)
        if src_node is None:
            raise FileNotFoundError(f"No such file: {src}")
        content = src_node.content if src_node.is_file() else ""
        # dst may be a dir
        if self.is_dir(dst):
            dst = dst.rstrip("/") + "/" + src_node.name
        self.write_file(dst, content, create_parents=True)

    def move(self, src: str, dst: str):
        src_node = self._get_node(src)
        if src_node is None:
            raise FileNotFoundError(f"No such file: {src}")
        if self.is_dir(dst):
            dst = dst.rstrip("/") + "/" + src_node.name
        dst = self.resolve(dst)
        dst_parent, dst_name = self._get_parent_and_name(dst)
        if dst_parent is None:
            raise PermissionError("Cannot move to root")
        # Remove from old parent
        src_parent, src_name = self._get_parent_and_name(self.resolve(src))
        del src_parent.children[src_name]
        src_node.name = dst_name
        src_node.parent = dst_parent
        dst_parent.children[dst_name] = src_node
        dst_parent.modified = time.time()

    def stat(self, path: str) -> dict:
        node = self._get_node(path)
        if node is None:
            raise FileNotFoundError(f"No such path: {path}")
        return {
            "name": node.name,
            "type": node.node_type,
            "size": node.size(),
            "owner": node.owner,
            "group": node.group,
            "permissions": node.permissions,
            "created": node.created,
            "modified": node.modified,
            "accessed": node.accessed,
        }

    def chmod(self, path: str, permissions: str):
        node = self._get_node(path)
        if node is None:
            raise FileNotFoundError(f"No such path: {path}")
        node.permissions = permissions
        node.modified = time.time()

    def chown(self, path: str, owner: str, group: str = None):
        node = self._get_node(path)
        if node is None:
            raise FileNotFoundError(f"No such path: {path}")
        node.owner = owner
        if group:
            node.group = group
        node.modified = time.time()

    # ── cwd ───────────────────────────────────────────────────────────────────

    def chdir(self, path: str):
        resolved = self.resolve(path)
        node = self._get_node(resolved)
        if node is None:
            raise FileNotFoundError(f"No such directory: {path}")
        if not node.is_dir():
            raise NotADirectoryError(f"Not a directory: {path}")
        self.cwd_path = resolved

    def getcwd(self) -> str:
        return self.cwd_path

    # ── Find / search ─────────────────────────────────────────────────────────

    def find(self, path: str, name_pattern: str = None, file_type: str = None) -> List[str]:
        results = []
        path = self.resolve(path)
        node = self._get_node(path)
        if node is None:
            return results
        self._find_recursive(node, path, name_pattern, file_type, results)
        return results

    def _find_recursive(self, node: VFSNode, current_path: str, pattern: str, ftype: str, results: list):
        import fnmatch
        for name, child in node.children.items():
            child_path = current_path.rstrip("/") + "/" + name
            match = True
            if pattern and not fnmatch.fnmatch(name, pattern):
                match = False
            if ftype and child.node_type != ftype:
                match = False
            if match:
                results.append(child_path)
            if child.is_dir():
                self._find_recursive(child, child_path, pattern, ftype, results)

    def grep(self, pattern: str, path: str, recursive: bool = False) -> List[Tuple[str, int, str]]:
        """Search for pattern in file(s). Returns list of (path, line_num, line)."""
        import re
        results = []
        node = self._get_node(path)
        if node is None:
            return results
        if node.is_file():
            self._grep_file(re.compile(pattern, re.IGNORECASE), path, node.content, results)
        elif node.is_dir() and recursive:
            for found_path in self.find(path, file_type="file"):
                f_node = self._get_node(found_path)
                if f_node:
                    self._grep_file(re.compile(pattern, re.IGNORECASE), found_path, f_node.content, results)
        return results

    def _grep_file(self, pattern, path, content, results):
        for i, line in enumerate(content.splitlines(), 1):
            if pattern.search(line):
                results.append((path, i, line))

    # ── Persistence ───────────────────────────────────────────────────────────

    def save(self, path: str = VFS_DATA_PATH):
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({
                    "root": self.root.to_dict(),
                    "cwd": self.cwd_path,
                    "version": "1.0",
                }, f, indent=2)
        except Exception as e:
            pass  # Silent fail - OS should still work

    def load(self, path: str = VFS_DATA_PATH) -> bool:
        try:
            if not os.path.exists(path):
                return False
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            self.root = VFSNode.from_dict(data["root"])
            self.cwd_path = data.get("cwd", "/home/user")
            return True
        except Exception:
            return False

    # ── Disk usage ────────────────────────────────────────────────────────────

    def du(self, path: str) -> int:
        node = self._get_node(path)
        if node is None:
            return 0
        return node.size()

    def df(self) -> dict:
        total = 64 * 1024 * 1024 * 1024  # 64 GB virtual disk
        used = self.root.size()
        return {
            "total": total,
            "used": used,
            "free": total - used,
            "percent": round(used / total * 100, 2),
        }

    def format_size(self, size: int) -> str:
        for unit in ["B", "KB", "MB", "GB", "TB"]:
            if size < 1024:
                return f"{size:.1f} {unit}"
            size /= 1024
        return f"{size:.1f} PB"
