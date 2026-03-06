import treelite as tl
import xgboost as xgb
import tl2cgen
import platform 


if __name__ == "__main__":
    TL_PATH = "xgb_binary_open_close_hand_model.tl" # pass path to standard treelite format model

    system = platform.system()
    
    if system == "Windows":
        LIB_PATH = "xgb_binary_open_close_hand_model.dll"
        TOOLCHAIN = "msvc"
    elif system == "Darwin":
        LIB_PATH = "xgb_binary_open_close_hand_model.dylib"
        TOOLCHAIN = "gcc"
    else:
        LIB_PATH = "xgb_binary_open_close_hand_model.so"
        TOOLCHAIN = "gcc"

    tl_model = tl.Model.deserialize(TL_PATH)

    tl2cgen.export_lib(
        tl_model,
        toolchain="msvc",
        libpath=LIB_PATH,
        params={"parallel_comp": 0}
    )