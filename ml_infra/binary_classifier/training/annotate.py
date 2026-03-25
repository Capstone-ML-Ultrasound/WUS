import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
from mediapipe.tasks.python.components.processors import classifier_options

model_path = "gesture_recognizer.task"
video_path = "IMG_7442.mov"

base_options = python.BaseOptions(model_asset_path=model_path)

options = vision.GestureRecognizerOptions(
    base_options=base_options,
    running_mode=vision.RunningMode.VIDEO,
    num_hands=1,
    canned_gesture_classifier_options=classifier_options.ClassifierOptions(
        display_names_locale="en",
        max_results=1,
        score_threshold=0.01,
        category_allowlist=["Closed_Fist", "Open_Palm"],
    ),
)

cap = cv2.VideoCapture(video_path)
print("opened:", cap.isOpened())

fps = cap.get(cv2.CAP_PROP_FPS)
print("fps:", fps)
if fps <= 0:
    fps = 30

with vision.GestureRecognizer.create_from_options(options) as recognizer:
    frame_idx = 0

    while True:
        ret, frame = cap.read()
        if not ret:
            print("done")
            break

        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        timestamp_ms = int((frame_idx / fps) * 1000)
        result = recognizer.recognize_for_video(mp_image, timestamp_ms)

        overlay_text = "No gesture"
        score_text = ""

        if result.gestures and len(result.gestures[0]) > 0:
            top = result.gestures[0][0]
            overlay_text = top.category_name
            score_text = f"{top.score:.3f}"

            print(result.gestures[0])
            print(frame_idx, top.category_name, top.score)
        else:
            print(frame_idx, "No gesture")

        # draw background box
        cv2.rectangle(frame, (10, 10), (300, 90), (0, 0, 0), -1)

        # draw label
        cv2.putText(
            frame,
            f"Pred: {overlay_text}",
            (20, 40),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.8,
            (0, 255, 0),
            2,
            cv2.LINE_AA,
        )

        # draw score
        if score_text:
            cv2.putText(
                frame,
                f"Score: {score_text}",
                (20, 75),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2,
                cv2.LINE_AA,
            )

        cv2.imshow("Gesture Recognition", frame)

        key = cv2.waitKey(int(1000 / fps)) & 0xFF
        if key == ord("q"):
            break

        frame_idx += 1

cap.release()
cv2.destroyAllWindows()