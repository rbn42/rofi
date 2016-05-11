#sudo apt install libxcb-xvmc0-dev -y
#sudo apt install libxcb-icccm4-dev -y
#sudo apt install libxcb-xinerama0-dev -y
#sudo apt install libxcb-ewmh-dev -y
#cd ~/git
#git clone https://github.com/rbn42/rofi.git
#cd rofi
autoreconf -i       
mkdir build
cd build
../configure
make
sudo make install
