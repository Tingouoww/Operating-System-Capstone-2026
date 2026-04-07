#!/usr/bin/env python3
import argparse
import os
import struct
import termios
import tty


BOOT_MAGIC = 0x544F4F42


def main() -> None:
    # 命令列參數：序列埠裝置路徑與要傳送的映像檔路徑
    parser = argparse.ArgumentParser(description="Send a kernel image over UART")
    parser.add_argument("device", help="Serial device path, for example /dev/ttyUSB0")
    parser.add_argument("image", help="Kernel image path, for example kernel.bin")
    args = parser.parse_args()

    # 讀入整個映像檔，後續會一次當成 payload 傳送
    with open(args.image, "rb") as image_file:
        kernel_data = image_file.read()

    # Boot protocol 的標頭：little-endian 格式的 <magic, payload_size>
    header = struct.pack("<II", BOOT_MAGIC, len(kernel_data))
    print(f"size:{len(kernel_data)}")
    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)

    try:
        # 先保存原始序列埠設定，離開前要還原
        attrs = termios.tcgetattr(fd)
        raw_attrs = termios.tcgetattr(fd)

        # 設成 raw mode，避免終端機行為（行編輯/轉譯）干擾二進位傳輸
        tty.setraw(fd, termios.TCSANOW)

        # 關閉軟體流控與 CR/LF 轉換
        raw_attrs[0] &= ~(termios.IXON | termios.IXOFF | termios.ICRNL | termios.INLCR)
        raw_attrs[1] &= ~termios.OPOST
        # 啟用接收端，並忽略 modem control lines
        raw_attrs[2] |= termios.CREAD | termios.CLOCAL
        # 關閉 echo/canonical/signal 處理，確保二進位資料原樣傳送
        raw_attrs[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG | termios.IEXTEN)
        termios.tcsetattr(fd, termios.TCSANOW, raw_attrs)

        # 依 bootloader 預期格式送出：[header][image]
        os.write(fd, header)
        os.write(fd, kernel_data)
        # 等待直到所有 bytes 實際送出完成
        termios.tcdrain(fd)
    finally:
        # 無論是否出錯，都要還原 TTY 設定
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        os.close(fd)


if __name__ == "__main__":
    main()
