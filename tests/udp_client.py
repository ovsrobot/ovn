#!/usr/bin/env python3
"""
Scapy UDP client with MTU/payload control for testing fragmented UDP traffic.

This module provides functionality to send fragmented UDP packets using Scapy,
allowing control over MTU, payload size, and fragmentation behavior.
"""
import time
import argparse
import sys
from scapy.all import IP, UDP, Raw, send, fragment

# Defaults for same logical switch topology
DEFAULT_SRC_IP = "172.16.1.3"
DEFAULT_DST_IP = "172.16.1.2"


def read_iface_mtu(ifname: str):
    """Return MTU of interface or None if not available."""
    try:
        path = f"/sys/class/net/{ifname}/mtu"
        with open(path, "r", encoding="utf-8") as f:
            return int(f.read().strip())
    except Exception:
        return None


def main():
    """Main function to parse arguments and send fragmented UDP packets."""
    parser = argparse.ArgumentParser(
        description="Scapy UDP client with MTU/payload control")
    parser.add_argument("-M", "--mtu", type=int, default=None,
        help="Target MTU; payload ~ mtu-28 (IPv4+UDP).")
    parser.add_argument("-B", "--payload-bytes", type=int, default=None,
        help="Total UDP payload bytes (override default).")
    parser.add_argument("-S", "--sport", type=int, default=4242,
        help="UDP source port")
    parser.add_argument("-D", "--dport", type=int, default=4242,
        help="UDP destination port")
    parser.add_argument("-s", "--src-ip", default=DEFAULT_SRC_IP,
        help="Source IPv4 address")
    parser.add_argument("-d", "--dst-ip", default=DEFAULT_DST_IP,
        help="Destination IPv4 address")
    parser.add_argument("-i", "--iface", default="client",
        help="Egress interface (default: 'client').")
    parser.add_argument("-I", "--interval", type=float, default=0.1,
        help="Seconds between sends.")
    parser.add_argument("-n", "--iterations", type=int, default=1,
        help="Number of datagrams to send.")
    args = parser.parse_args()

    # Derive fragment size: for link MTU M, fragsize should typically be M-20
    # (IP header)
    if args.mtu is not None:
        fragsize = max(68, int(args.mtu) - 20)
    else:
        fragsize = 1480  # default for 1500 MTU

    # Build payload; size can be user-controlled or synthesized to ensure
    # fragmentation
    if args.payload_bytes is not None:
        payload_len = max(0, int(args.payload_bytes))
        payload = b"x" * payload_len
    elif args.mtu is not None:
        # Ensure payload exceeds fragsize-8 (UDP header) so we actually
        # fragment.
        base_len = max(0, int(args.mtu) - 28)
        min_len_to_fragment = max(0, fragsize - 8 + 1)
        payload_len = (base_len if base_len > min_len_to_fragment else
                       fragsize * 2)
        payload = b"x" * payload_len
    else:
        # No explicit size; synthesize a payload big enough to fragment at
        # default fragsize
        payload_len = fragsize * 2
        payload = b"x" * payload_len

    # Construct full IP/UDP packet and fragment it explicitly
    ip_layer = IP(src=args.src_ip, dst=args.dst_ip)
    udp_layer = UDP(sport=args.sport, dport=args.dport)
    full_packet = ip_layer / udp_layer / Raw(load=payload)
    frags = fragment(full_packet, fragsize=fragsize)

    total_fragments_per_send = len(frags)
    for _ in range(max(0, args.iterations)):
        try:
            send(frags, iface=args.iface, return_packets=True, verbose=False)
        except OSError as e:
            # Errno 90: Message too long (likely iface MTU < chosen --mtu)
            if getattr(e, "errno", None) == 90:
                iface_mtu = read_iface_mtu(args.iface)
                mtu_note = (f"iface_mtu={iface_mtu}" if iface_mtu is not None
                            else "iface_mtu=unknown")
                print("ERROR: packet exceeds interface MTU. "
                    f"iface={args.iface} {mtu_note} chosen_mtu="
                    f"{args.mtu if args.mtu is not None else 1500} "
                    f"fragsize={fragsize}. Set --mtu to the interface MTU.",
                    file=sys.stderr)
                sys.exit(2)
            raise
        time.sleep(args.interval)

    # Summary
    mtu_print = args.mtu if args.mtu is not None else 1500
    total_frags = total_fragments_per_send * max(0, args.iterations)
    print(f"payload_bytes={payload_len} mtu={mtu_print} fragsize={fragsize} "
        f"fragments_per_datagram={total_fragments_per_send} "
        f"total_fragments_sent={total_frags}")


if __name__ == "__main__":
    main()
    sys.exit(0)
