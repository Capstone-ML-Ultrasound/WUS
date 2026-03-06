import xgboost as xgb
import treelite as tl 

if __name__ == "__main__":
    MODEL_PATH = "xgb_binary_open_close_hand_model.json"  # xgb format
    EXPORT_PATH = "xgb_binary_open_close_hand_model.tl"   # standard treelite format 

    tl_model = tl.frontend.load_xgboost_model(MODEL_PATH)   
    tl_model.serialize(EXPORT_PATH)