from collections import deque
import numpy as np
import tl2cgen
import numpy as np
import time


class InferenceEngine:
    lib_path: str = "./xgb_binary_open_close_hand_model.dll"
    buff_in_max_len: int = 1000
    buff_out_max_len: int = 1000
    kafka_cfg: dict = {}

    def __init__(self, lib_path: str | None = None, kafka_cfg: dict | None = None):
        if lib_path is None:
            lib_path = type(self).lib_path
        if kafka_cfg is None:
            kafka_cfg = {}

        self.predictor = tl2cgen.Predictor(lib_path, nthread=1)
        self.kafka_cfg = kafka_cfg

        # self.in_buff = deque(maxlen=type(self).buff_in_max_len)
        # self.out_buff = deque(maxlen=type(self).buff_out_max_len)

    def predict(self, data: np.ndarray) -> np.ndarray:
        return self.predictor.predict(data)


if __name__ == "__main__":
    engine = InferenceEngine()
    test_input_arr = np.load("X_pca.npy")
    print(test_input_arr.shape) # (1000,200) row x col
    
    for i in range(1000):
        input_data = tl2cgen.DMatrix(np.array([test_input_arr[i]]).reshape(1, -1))  # shape (1, 200)
        
        time_start = time.perf_counter()
        output = engine.predict(input_data)
        time_end = time.perf_counter()
        print(f"Pred: {output}")
        print(f"Time taken for prediction: {time_end - time_start:.6f} seconds")

    # This is only a test, implement read from kafka topic -> predict -> write to output kafka
    

    