import socket
import struct
import numpy as np
import mediapipe as mp
import os

# initial MediaPipe Hand
mp_hands = mp.solutions.hands
hands = mp_hands.Hands(
    static_image_mode=False,
    max_num_hands=1,
    min_detection_confidence=0.5,
    min_tracking_confidence=0.5
)

# Socket path
SOCKET_PATH = "/tmp/cam.sock"

def start_client():
    # init UDS Socket
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    
    print(f"Connecting to {SOCKET_PATH}...")
    try:
        client.connect(SOCKET_PATH)
    except socket.error as msg:
        print(f"Link error: {msg}")
        return

    print("Link established, waiting for data...")

    # struct format：i (int32, frame_idx), 65536s (65536 bytes, img)
    header_struct = struct.Struct('i')
    img_size = 256 * 256

    try:
        while True:
            # frame_idx
            idx_data = client.recv(4)
            if not idx_data: break
            frame_idx = header_struct.unpack(idx_data)[0]

            # img_data
            img_data = b''
            while len(img_data) < img_size:
                chunk = client.recv(img_size - len(img_data))
                if not chunk: break
                img_data += chunk
            
            # transform to numpy array
            y_img = np.frombuffer(img_data, dtype=np.uint8).reshape(256, 256)
            
            # MediaPipe Hand Detection
            rgb_img = cv2.cvtColor(y_img, cv2.COLOR_GRAY2RGB)
            results = hands.process(rgb_img)

            # format: frame_idx (int) + 42 floats (21 landmarks x 2 coordinates)
            coords = [0.0] * 42
            if results.multi_hand_landmarks:
                for hand_landmarks in results.multi_hand_landmarks:
                    for i, lm in enumerate(hand_landmarks.landmark):
                        coords[i*2] = lm.x
                        coords[i*2+1] = lm.y
            
            # 'i' for int, '42f' for 42 floats
            packet = struct.pack('i42f', frame_idx, *coords)
            client.sendall(packet)

            if frame_idx % 30 == 0:
                print(f"Processed frame {frame_idx}")

    finally:
        print("Closing connection...")
        client.close()

if __name__ == "__main__":
    import cv2
    start_client()