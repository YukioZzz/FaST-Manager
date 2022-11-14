# call from repo root

FROM nvidia/cuda:10.0-devel AS build

WORKDIR /tmp/build

COPY . Gemini
RUN apt-key adv --fetch-keys https://developer.download.nvidia.com/compute/cuda/repos/ubuntu1804/x86_64/3bf863cc.pub
RUN apt-key adv --fetch-keys https://developer.download.nvidia.com/compute/machine-learning/repos/ubuntu1804/x86_64/7fa2af80.pub

RUN apt update && \
    echo "yes" | apt-get install libzmq3-dev && \
    apt install -y git && \
    cd Gemini && \
    make -C src DEBUG=1 

FROM python:3.8.1-buster

RUN pip3 install inotify

ENV NVIDIA_VISIBLE_DEVICES      all
ENV NVIDIA_DRIVER_CAPABILITIES  utility

COPY --from=build /tmp/build/Gemini/docker/kubeshare-gemini-scheduler/launcher-multigpus.sh /launcher-multigpus.sh
COPY --from=build /tmp/build/Gemini/docker/kubeshare-gemini-scheduler/launcher.py /launcher.py
COPY --from=build /tmp/build/Gemini/bin/gem-schd /gem-schd
COPY --from=build /tmp/build/Gemini/bin/gem-pmgr /gem-pmgr

CMD ["/launcher-multigpus.sh", "/kubeshare/scheduler/config", "/kubeshare/scheduler/podmanagerport"]
