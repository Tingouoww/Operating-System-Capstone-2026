#!/usr/bin/env python3
import argparse
import os
import struct
import termios
import tty


BOOT_MAGIC = 0x544F4F42


def main() -> None:
    parser = argparse.ArgumentParser(description="Send a kernel image over UART")
    parser.add_argument("device", help="Serial device path, for example /dev/ttyUSB0")
    parser.add_argument("image", help="Kernel image path, for example kernel.bin")
    args = parser.parse_args()

    with open(args.image, "rb") as image_file:
        kernel_data = image_file.read()

    header = struct.pack("<II", BOOT_MAGIC, len(kernel_data))
    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)

    try:
        attrs = termios.tcgetattr(fd)
        raw_attrs = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)

        raw_attrs[0] &= ~(termios.IXON | termios.IXOFF | termios.ICRNL | termios.INLCR)
        raw_attrs[1] &= ~termios.OPOST
        raw_attrs[2] |= termios.CREAD | termios.CLOCAL
        raw_attrs[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG | termios.IEXTEN)
        termios.tcsetattr(fd, termios.TCSANOW, raw_attrs)

        os.write(fd, header)
        os.write(fd, kernel_data)
        termios.tcdrain(fd)
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        os.close(fd)


if __name__ == "__main__":
    main()
