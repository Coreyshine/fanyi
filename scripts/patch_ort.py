#!/usr/bin/env python3
# 拍平 onnxruntime 嵌套目录并校验（CI 用）
import os, shutil
base = 'third_party/onnxruntime'
for d in os.listdir(base):
    p = os.path.join(base, d)
    if os.path.isdir(p) and d.startswith('onnxruntime-'):
        for f in os.listdir(p):
            src, dst = os.path.join(p, f), os.path.join(base, f)
            if os.path.exists(dst):
                shutil.rmtree(dst) if os.path.isdir(dst) else os.remove(dst)
            shutil.move(src, dst)
        os.rmdir(p)
assert os.path.exists(os.path.join(base, 'include', 'onnxruntime_cxx_api.h')), 'include missing'
assert os.path.isdir(os.path.join(base, 'lib')), 'lib missing'
print('ORT ready:', os.listdir(os.path.join(base, 'lib')))
