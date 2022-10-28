# Personal Notes

# Building linux (or mac)

    sudo apt install git
    sudo apt install cmake

    // libaries used
    sudo apt-get install libusb-1.0-0-dev
    sudo apt-get install libasound2-dev (linux)
    sudo apt-get install libcairo2-dev (linux)
    mkdir build
    cd build
    cmake ..
    make

sudo cp -f release/lib/libmec-\* /usr/local/MEC/

Debug with: std::cerr << "midiCC unlearn" << num << " " << modParamId\_ << std::endl;
