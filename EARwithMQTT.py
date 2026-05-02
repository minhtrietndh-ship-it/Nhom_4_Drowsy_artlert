import cv2
import mediapipe as mp
from scipy.spatial import distance
import paho.mqtt.client as mqtt
import time

# ===== MQTT SETUP =====
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
MQTT_TOPIC = "esp32/sleep_status"

client = mqtt.Client()
client.connect(MQTT_BROKER, MQTT_PORT, 60)

# ===== EAR function =====
def eye_aspect_ratio(eye):
    A = distance.euclidean(eye[1], eye[5])
    B = distance.euclidean(eye[2], eye[4])
    C = distance.euclidean(eye[0], eye[3])
    return (A + B) / (2.0 * C)

# ===== MediaPipe =====
mp_face_mesh = mp.solutions.face_mesh
face_mesh = mp_face_mesh.FaceMesh(
    max_num_faces=1,
    refine_landmarks=False,
    min_detection_confidence=0.5,
    min_tracking_confidence=0.5
)

LEFT_EYE_IDX = [33, 160, 158, 133, 153, 144]
RIGHT_EYE_IDX = [362, 385, 387, 263, 373, 380]

# ===== ESP32-CAM STREAM =====
url = "http://192.168.70.69:81/stream"
cap = cv2.VideoCapture(url)
cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

# ===== PARAM =====
EAR_THRESH = 0.3

SLEEP_FRAMES = 10   # cần 10 frame liên tiếp để ngủ
WAKE_FRAMES = 5     # cần 5 frame để tỉnh

sleep_counter = 0
wake_counter = 0

prev_status = "AWAKE"
current_status = "AWAKE"

last_send = 0
frame_count = 0
results = None   # giữ kết quả cũ

# ===== LOOP =====
while True:
    # giảm lag
    for _ in range(2):
        cap.grab()

    ret, frame = cap.read()
    if not ret:
        break

    frame = cv2.resize(frame, (320, 240))
    h, w = frame.shape[:2]
    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

    # giảm tải CPU nhưng KHÔNG reset results
    frame_count += 1
    if frame_count % 2 == 0:
        results = face_mesh.process(rgb)

    # mặc định giữ trạng thái cũ
    current_status = prev_status

    if results and results.multi_face_landmarks:
        for face_landmarks in results.multi_face_landmarks:

            left_eye = []
            right_eye = []

            for idx in LEFT_EYE_IDX:
                x = int(face_landmarks.landmark[idx].x * w)
                y = int(face_landmarks.landmark[idx].y * h)
                left_eye.append((x, y))

            for idx in RIGHT_EYE_IDX:
                x = int(face_landmarks.landmark[idx].x * w)
                y = int(face_landmarks.landmark[idx].y * h)
                right_eye.append((x, y))

            leftEAR = eye_aspect_ratio(left_eye)
            rightEAR = eye_aspect_ratio(right_eye)
            ear = (leftEAR + rightEAR) / 2.0

            # vẽ điểm mắt
            for (x, y) in left_eye + right_eye:
                cv2.circle(frame, (x, y), 1, (0, 255, 0), -1)

            # ===== LOGIC CHỐNG RUNG =====
            if ear < EAR_THRESH:
                sleep_counter += 1
                wake_counter = 0

                if sleep_counter >= SLEEP_FRAMES:
                    current_status = "SLEEP"

            else:
                wake_counter += 1
                sleep_counter = 0

                if wake_counter >= WAKE_FRAMES:
                    current_status = "AWAKE"

            # hiển thị
            cv2.putText(frame, f"EAR: {ear:.2f}", (10, 20),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

    # ===== MQTT (chỉ gửi khi đổi trạng thái) =====
    if current_status != prev_status and time.time() - last_send > 1:
        
        client.publish(MQTT_TOPIC, current_status)
        print("Send:", current_status)
        prev_status = current_status
        last_send = time.time()

    # hiển thị trạng thái
    cv2.putText(frame, current_status, (50, 100),
                cv2.FONT_HERSHEY_SIMPLEX, 1,
                (0, 0, 255) if current_status == "SLEEP" else (0, 255, 0), 2)

    cv2.imshow("ESP32-CAM EAR", frame)

    if cv2.waitKey(1) & 0xFF == 27:
        break

cap.release()
cv2.destroyAllWindows()