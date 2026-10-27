import hid
import time

VID = 0x1209
PID = 0x0001

def main():
    print(f"Looking for HID Device VID:0x{VID:04X} PID:0x{PID:04X}")

    device = hid.device()
    try:
        device.open(VID, PID)
        device.set_nonblocking(True)
        print("Successfully connected to the USB Gadget!\n")
    except Exception as e:
        print(f"Failed to open device: {e}")
        return

    try:
        # Create a 64-byte payload.
        # Note: hidapi on some OS requires an extra leading 0x00 for the implicit Report ID (0).
        # Making the list length 65 ensures the 64 bytes get sent cleanly.
        data_to_send = [0x00] +[i % 256 for i in range(64)]

        # Modify some bytes to verify our echo logic
        data_to_send[1] = 0xAA
        data_to_send[2] = 0xBB
        data_to_send[64] = 0xCC

        print("Sending 64 bytes out...")
        device.write(data_to_send)

        # Wait for the device C program to echo the data back
        time.sleep(0.5)

        # Attempt to read up to 64 bytes back with a 1000ms timeout
        read_data = device.read(64, timeout_ms=1000)

        if read_data:
            print(f"Received {len(read_data)} bytes back:")
            # Convert to hex representation for easy reading
            hex_data = " ".join([f"{byte:02X}" for byte in read_data])
            print(hex_data)
        else:
            print("Did not receive a response from the device.")

    except Exception as e:
        print(f"Communication error: {e}")
    finally:
        device.close()
        print("\nConnection closed.")

if __name__ == "__main__":
    main()

