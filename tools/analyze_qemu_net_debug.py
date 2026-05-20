#!/usr/bin/env python3
import argparse
import json
import os
import re
import struct
import time


SESSION_ID = "225346"


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--pcap", required=True)
    parser.add_argument("--debug-log", required=True)
    parser.add_argument("--run-id", default="cli-kpkg-sync")
    return parser.parse_args()


def emit(debug_log, run_id, hypothesis_id, location, message, data):
    entry = {
        "sessionId": SESSION_ID,
        "runId": run_id,
        "hypothesisId": hypothesis_id,
        "location": location,
        "message": message,
        "data": data,
        "timestamp": int(time.time() * 1000),
    }
    with open(debug_log, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry, separators=(",", ":")) + "\n")


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def fmt_ip(raw):
    return ".".join(str(b) for b in raw)


def fmt_mac(raw):
    return ":".join(f"{b:02x}" for b in raw)


def parse_serial(serial_path):
    text = ""
    if os.path.exists(serial_path):
        with open(serial_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()

    mac_match = re.search(r"\[E1000\] MAC:\s+(.+)", text)
    guest_mac = "52:54:00:12:34:56"
    if mac_match:
        octets = re.findall(r"0x([0-9A-Fa-f]+)", mac_match.group(1))
        if len(octets) >= 6:
            guest_mac = ":".join(f"{int(v, 16) & 0xFF:02x}" for v in octets[:6])

    tcpip_marker = text.find("[TCPIP] Init...")
    pre_tcp_text = text if tcpip_marker < 0 else text[:tcpip_marker]

    serial_data = {
        "guest_mac": guest_mac,
        "syn_timeouts": len(re.findall(r"Connect TIMEOUT \(no SYN-ACK in 10s\)", text)),
        "syn_sent": len(re.findall(r"SYN sent, waiting for SYN-ACK", text)),
        "arp_resolved": len(re.findall(r"\[TCP\] ARP resolved OK", text)),
        "arp_replies": len(re.findall(r"\[ARP\] REPLY", text)),
        "tcp_rx_zero_timeouts": len(re.findall(r"tcp_rx=0", text)),
        "pre_tcp_force_text": "Display: forced text mode from boot option" in pre_tcp_text,
        "pre_tcp_no_framebuffer": "Display: No framebuffer! Showing diagnostics on VGA text." in pre_tcp_text,
        "pre_tcp_gui_zero_backbuffer": "GUI::UpdateBackbuffer: w=0 h=0 bpp=0 pitch=0" in pre_tcp_text,
        "pre_tcp_wallpaper_loads": len(re.findall(r"Loading embedded wallpaper", pre_tcp_text)),
        "pre_tcp_keyboard_init_count": len(re.findall(r"Keyboard: Initializing Enhanced Driver", pre_tcp_text)),
        "pre_tcp_mouse_defaults_failed": len(re.findall(r"Mouse: Defaults Failed", pre_tcp_text)),
        "pre_tcp_mouse_streaming_failed": len(re.findall(r"Mouse: Streaming Failed", pre_tcp_text)),
        "pre_tcp_vconsole_gui_active": "VConsole: 7 virtual consoles ready (active=tty7/GUI)" in pre_tcp_text,
        "kpkg_sync_invoked": "cli> kpkg sync" in text,
        "kpkg_sync_misparsed_as_install": "Installing sync from " in text,
        "kpkg_sync_missing_package": "Package was not found in the local or remote index." in text,
        "text": text,
    }
    return serial_data


def read_pcap_packets(path):
    if not os.path.exists(path):
        return []
    with open(path, "rb") as fh:
        header = fh.read(24)
        if len(header) < 24:
            return []

        magic_le = struct.unpack("<I", header[:4])[0]
        magic_be = struct.unpack(">I", header[:4])[0]
        if magic_le in (0xA1B2C3D4, 0xA1B23C4D):
            endian = "<"
        elif magic_be in (0xA1B2C3D4, 0xA1B23C4D):
            endian = ">"
        else:
            return []

        packets = []
        while True:
            pkt_hdr = fh.read(16)
            if len(pkt_hdr) < 16:
                break
            _ts_sec, _ts_usec, incl_len, _orig_len = struct.unpack(endian + "IIII", pkt_hdr)
            data = fh.read(incl_len)
            if len(data) < incl_len:
                break
            packets.append(data)
        return packets


def parse_packets(raw_packets, guest_mac):
    parsed = []
    guest_mac = guest_mac.lower()
    for frame in raw_packets:
        if len(frame) < 14:
            continue
        dst_mac = fmt_mac(frame[0:6])
        src_mac = fmt_mac(frame[6:12])
        ethertype = struct.unpack("!H", frame[12:14])[0]
        direction = "other"
        if src_mac == guest_mac:
            direction = "out"
        elif dst_mac == guest_mac:
            direction = "in"

        pkt = {
            "direction": direction,
            "src_mac": src_mac,
            "dst_mac": dst_mac,
            "ethertype": ethertype,
            "frame_len": len(frame),
            "raw": frame,
        }

        if ethertype == 0x0806 and len(frame) >= 42:
            arp = frame[14:42]
            pkt["arp"] = {
                "opcode": struct.unpack("!H", arp[6:8])[0],
                "sender_mac": fmt_mac(arp[8:14]),
                "sender_ip": fmt_ip(arp[14:18]),
                "target_ip": fmt_ip(arp[24:28]),
            }
        elif ethertype == 0x0800 and len(frame) >= 34:
            ip = frame[14:]
            ihl = (ip[0] & 0x0F) * 4
            if len(ip) < ihl or ihl < 20:
                parsed.append(pkt)
                continue
            total_length = struct.unpack("!H", ip[2:4])[0]
            total_length = min(total_length, len(ip))
            ip_header = ip[:ihl]
            ip_payload = ip[ihl:total_length]
            proto = ip[9]
            pkt["ipv4"] = {
                "src_ip": fmt_ip(ip[12:16]),
                "dst_ip": fmt_ip(ip[16:20]),
                "protocol": proto,
                "ihl": ihl,
                "total_length": total_length,
                "checksum_ok": checksum(ip_header) == 0,
            }
            if proto == 6 and len(ip_payload) >= 20:
                tcp_offset = ((ip_payload[12] >> 4) & 0x0F) * 4
                if len(ip_payload) >= tcp_offset and tcp_offset >= 20:
                    tcp_header = ip_payload[:tcp_offset]
                    tcp_payload = ip_payload[tcp_offset:]
                    pseudo = (
                        ip[12:16]
                        + ip[16:20]
                        + b"\x00"
                        + bytes([6])
                        + struct.pack("!H", len(ip_payload))
                    )
                    pkt["tcp"] = {
                        "src_port": struct.unpack("!H", tcp_header[0:2])[0],
                        "dst_port": struct.unpack("!H", tcp_header[2:4])[0],
                        "seq": struct.unpack("!I", tcp_header[4:8])[0],
                        "ack": struct.unpack("!I", tcp_header[8:12])[0],
                        "flags": tcp_header[13],
                        "window": struct.unpack("!H", tcp_header[14:16])[0],
                        "checksum_ok": checksum(pseudo + tcp_header + tcp_payload) == 0,
                        "payload_len": len(tcp_payload),
                    }
        parsed.append(pkt)
    return parsed


def main():
    args = parse_args()
    serial = parse_serial(args.serial)
    packets = parse_packets(read_pcap_packets(args.pcap), serial["guest_mac"])

    outbound_syns = [
        p for p in packets
        if p.get("direction") == "out"
        and p.get("ipv4", {}).get("protocol") == 6
        and p.get("tcp", {}).get("dst_port") == 80
        and p.get("tcp", {}).get("flags", 0) & 0x02
    ]
    inbound_tcp = [
        p for p in packets
        if p.get("direction") == "in"
        and p.get("ipv4", {}).get("protocol") == 6
    ]
    inbound_synacks = [
        p for p in inbound_tcp
        if (p.get("tcp", {}).get("flags", 0) & 0x12) == 0x12
    ]
    inbound_arp = [
        p for p in packets
        if p.get("direction") == "in"
        and p.get("ethertype") == 0x0806
    ]
    outbound_arp = [
        p for p in packets
        if p.get("direction") == "out"
        and p.get("ethertype") == 0x0806
    ]

    syn_sample = outbound_syns[0] if outbound_syns else {}

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H1",
        "tools/analyze_qemu_net_debug.py:203",
        "Packet capture summary",
        {
            "guestMac": serial["guest_mac"],
            "pcapPackets": len(packets),
            "outboundArp": len(outbound_arp),
            "inboundArp": len(inbound_arp),
            "outboundSyn": len(outbound_syns),
            "inboundTcp": len(inbound_tcp),
            "inboundSynAck": len(inbound_synacks),
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H1",
        "tools/analyze_qemu_net_debug.py:220",
        "First outbound SYN validation",
        {
            "present": bool(syn_sample),
            "srcIp": syn_sample.get("ipv4", {}).get("src_ip"),
            "dstIp": syn_sample.get("ipv4", {}).get("dst_ip"),
            "srcPort": syn_sample.get("tcp", {}).get("src_port"),
            "dstPort": syn_sample.get("tcp", {}).get("dst_port"),
            "flags": syn_sample.get("tcp", {}).get("flags"),
            "ipChecksumOk": syn_sample.get("ipv4", {}).get("checksum_ok"),
            "tcpChecksumOk": syn_sample.get("tcp", {}).get("checksum_ok"),
            "frameLen": syn_sample.get("frame_len"),
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H2",
        "tools/analyze_qemu_net_debug.py:240",
        "Inbound TCP response observation",
        {
            "inboundTcpPackets": len(inbound_tcp),
            "inboundSynAckPackets": len(inbound_synacks),
            "firstInboundFlags": inbound_tcp[0]["tcp"]["flags"] if inbound_tcp else None,
            "firstInboundSrcIp": inbound_tcp[0]["ipv4"]["src_ip"] if inbound_tcp else None,
            "firstInboundDstIp": inbound_tcp[0]["ipv4"]["dst_ip"] if inbound_tcp else None,
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H3",
        "tools/analyze_qemu_net_debug.py:254",
        "ARP and receive-path health",
        {
            "serialArpResolved": serial["arp_resolved"],
            "serialArpReplies": serial["arp_replies"],
            "pcapInboundArp": len(inbound_arp),
            "pcapOutboundArp": len(outbound_arp),
            "serialTcpRxZeroTimeouts": serial["tcp_rx_zero_timeouts"],
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H4",
        "tools/analyze_qemu_net_debug.py:268",
        "Serial TCP connect outcome",
        {
            "synSent": serial["syn_sent"],
            "synTimeouts": serial["syn_timeouts"],
            "pcapAvailable": os.path.exists(args.pcap),
            "serialPath": os.path.basename(args.serial),
            "pcapPath": os.path.basename(args.pcap),
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H5",
        "tools/analyze_qemu_net_debug.py:290",
        "Pre-TCP text-mode GUI path",
        {
            "forceTextMode": serial["pre_tcp_force_text"],
            "loggedNoFramebufferError": serial["pre_tcp_no_framebuffer"],
            "guiZeroBackbuffer": serial["pre_tcp_gui_zero_backbuffer"],
            "wallpaperLoadsBeforeTcp": serial["pre_tcp_wallpaper_loads"],
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H6",
        "tools/analyze_qemu_net_debug.py:305",
        "Pre-TCP keyboard init count",
        {
            "keyboardInitCountBeforeTcp": serial["pre_tcp_keyboard_init_count"],
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H7",
        "tools/analyze_qemu_net_debug.py:316",
        "Pre-TCP mouse init degradation",
        {
            "mouseDefaultsFailedBeforeTcp": serial["pre_tcp_mouse_defaults_failed"],
            "mouseStreamingFailedBeforeTcp": serial["pre_tcp_mouse_streaming_failed"],
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H8",
        "tools/analyze_qemu_net_debug.py:328",
        "Pre-TCP virtual console selection",
        {
            "vconsoleGuiActiveBeforeTcp": serial["pre_tcp_vconsole_gui_active"],
        },
    )
    #endregion

    #region agent log
    emit(
        args.debug_log,
        args.run_id,
        "H9",
        "tools/analyze_qemu_net_debug.py:343",
        "kpkg sync command routing",
        {
            "kpkgSyncInvoked": serial["kpkg_sync_invoked"],
            "misparsedAsInstallSync": serial["kpkg_sync_misparsed_as_install"],
            "missingPackageSync": serial["kpkg_sync_missing_package"],
        },
    )
    #endregion


if __name__ == "__main__":
    main()
