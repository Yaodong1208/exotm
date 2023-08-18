# Dockerfile to build llvm-15 developer image
FROM ubuntu:jammy

# Apply all updates
RUN apt-get update -y
RUN apt-get upgrade -y

# Install basic C++ and Python development tools
RUN DEBIAN_FRONTEND=noninteractive apt install -y build-essential cmake g++-multilib pip

# Install LLVM 15
RUN DEBIAN_FRONTEND=noninteractive apt install -y wget gnupg gnupg2 gnupg1 lsb-release software-properties-common
RUN DEBIAN_FRONTEND=noninteractive wget https://apt.llvm.org/llvm.sh
RUN DEBIAN_FRONTEND=noninteractive chmod +x llvm.sh
RUN DEBIAN_FRONTEND=noninteractive ./llvm.sh 15
RUN DEBIAN_FRONTEND=noninteractive rm ./llvm.sh

# Install Python charting tools
RUN pip3 install --no-cache-dir numpy matplotlib

# Set the working directory
WORKDIR /root

# To use this Dockerfile
# 1 - Make an image named exotm_ae
#     - Go to the folder where this Dockerfile exists
#     - sudo docker build -t exotm_ae .
#       - Note: you don't need the 'sudo' part on Windows
#     - The resulting image size will be about 1.64 GB
# 2 - Go to top level folder
# 3 - Launch an interactive container, and mount your working folder
#     - sudo docker run --privileged --rm -v $(pwd):/root -it exotm_ae
# 4 - When your terminal starts up:
#     - You will be logged in as root, and in the `/root` folder
#     - You should see your exotm folder's contents in there
#     - Type 'make' to run all experiments and build all charts
