# FaST-Manager

## About

FaST-Manager enables fine-grained spatio-temporal GPU sharing. It has two main parts, one is the hook library and the other serve as a node-daemon backend. It records the consumed time quota, space partition and memory, and control the access of the function instance to gpu directly.

SM Partition will be considered, and multiple tokens might be given out at the same time as long as the resources can be shared without interference among instances.
## Build
Usually, this project should be deployed with the FaST in the context of kuberenetes. So a docker image wil be built directly with the following command:

```
docker build -f docker/kubeshare-gemini-scheduler/Dockerfile -t yukiozhu/kubeshare-gemini-scheduler:mps .
docker build -f docker/kubeshare-gemini-hook-init/Dockerfile -t yukiozhu/kubeshare-gemini-hook-init:mps .
docker push ...
```

We can still compile it locally with the following command:

```
make [CUDA_PATH=/path/to/cuda/installation] [PREFIX=/place/to/install] [DEBUG=1]
```
