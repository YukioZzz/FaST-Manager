import signal
import ctypes

lib = ctypes.CDLL("/kubeshare/library/libgemhook.so.1")

def sigint_handler(signum, frame):
    print("Received SIGINT, exiting...")
    lib._Z13sigintHandleri(signum)

def sigcont_handler(signum, frame):
    print("Received SIGCONT, exiting...")
    lib._Z14sigcontHandleri(signum)

signal.signal(signal.SIGINT, sigint_handler)
signal.signal(signal.SIGCONT, sigcont_handler)

