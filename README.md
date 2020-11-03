# ESP32 sniffer

<p align="center">
  <img alt="ETS" src="https://user-images.githubusercontent.com/25306548/70620809-7a028600-1c18-11ea-939c-5358bb2c61f7.png" height="140" />
  <p align="center">
    <a href="https://docs.espressif.com/projects/esp-idf/en/stable/get-started/"><img src="https://readthedocs.com/projects/espressif-esp-idf/badge/?version=stable"></a>
    <a href=""><img alt="Software License" src="https://img.shields.io/cran/l/devtools"></a>
    <a href="https://youtu.be/NMywky9Ts_w"><img alt="Demo" src="https://img.shields.io/badge/demo-youtube-blue.svg"></a>
    <a href="https://twitter.com/giannofederico"><img alt="Twitter" src="https://img.shields.io/twitter/follow/giannofederico.svg?style=social&label=Follow"></a>
  </p>
</p>

This project is using [Espressif IoT Development Framework](https://github.com/espressif/esp-idf) (ESP-IDF) and has been tested on ESP-WROOM-32 module.

An overview of the full project can be found [here](https://j4nn0.github.io/doc/ets_presentation.pdf).

This firmware is written with the purpose to sniff probe request packets sent by smartphones that are looking for Wi-Fi connection. When the Wi-Fi connection is active on the smartphone, it will send in broadcast (in all channels frequencies) a probe request messagge asking if there is some free Wi-Fi or some known ones (i.e. the smartphone knows the password for that Wi-Fi). 
The ESP will sniff it and extract different types of informations.

From each sniffed packet the following informations is taken:

- MAC of the smartphone that has sent the request
- SSID of the wifi to which the request is sent
- The timestamp when the request is sent
- RSSI
- Sequence Number
- HT Capabilities Info

After each minute these informations are sent to a [server](https://github.com/ETS-PoliTO/ETS-Server) and processed.  
Finally, it is possible to see the processed informations (real time location of the smartphones, smartphone frequency, etc.) through a [GUI](https://github.com/ETS-PoliTO/GUI-Application).

### Demo 
[![Watch the video](https://img.youtube.com/vi/NMywky9Ts_w/maxresdefault.jpg)](https://youtu.be/NMywky9Ts_w)

# Table of Contents
- [Firmware development](https://github.com/ETS-PoliTO/esp32-sniffer#firmware-overview)
- [ESP-IDF environment configuration](https://github.com/ETS-PoliTO/esp32-sniffer#esp-idf-environment-configuration)
- [Usage](https://github.com/ETS-PoliTO/esp32-sniffer#usage)
- [Configuration file](https://github.com/ETS-PoliTO/esp32-sniffer#configuration-file)
- [Components](https://github.com/ETS-PoliTO/esp32-sniffer#components)
- [Resources](https://github.com/ETS-PoliTO/esp32-sniffer#resources)

# Firmware Overview

The firmware consits in two main threads:

- Sniffer task
    
    - Sniff PROBE REQUEST packet and save the infomations described above into a file

- Wi-Fi task

    - Each minute take the informations saved by the **sniffer task** and send it to the server
    - A lock is used in order to manage critical section for I/O operations in the file

The ESP32 is configured in WIFI_MODE_APSTA mode: it creates *"soft-AP and station control block"* and start *"soft-AP and station"*. Thanks to this, the ESP32 is able to sniff and send informations to the server at the same time avoid losing packets information while sending data.

# ESP-IDF environment configuration

1. Setup Toolchain

	- [Windows](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/windows-setup.html)
	- [Linux](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/linux-setup.html)
	- [Mac OS](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/macos-setup.html)
	
2.  Get ESP-IDF

	Besides the toolchain (that contains programs to compile and build the application), you also need ESP32 specific API / libraries.

		cd ~/esp
		git clone --recursive https://github.com/espressif/esp-idf.git
		
3. Setup Path to ESP-IDF

	The toolchain programs access ESP-IDF using IDF_PATH environment variable.
	This variable should be set up on your PC, otherwise projects will not build.
	
	- [Windows](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/add-idf_path-to-profile.html#add-idf-path-to-profile-windows)
	- [Linux & Mac OS](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/add-idf_path-to-profile.html#add-idf-path-to-profile-linux-macos)
	
4. Install the Required Python Packages

		python -m pip install --user -r $IDF_PATH/requirements.txt

Check the [official site](https://esp-idf.readthedocs.io/en/latest/get-started/index.html) for more info.

# Usage
	
1. Make sure you have exported the path
	
		export IDF_PATH=~/esp/esp-idf
		export PATH=$PATH:$HOME/esp/xtensa-esp32-elf/bin

2. Clone the repo
	
		git clone https://github.com/ETS-PoliTO/esp32-sniffer.git

3. Establish serial connection between ESP32 and your PC
	
		make menuconfig

	 Go to `Serial flasher config`, then `Default serial port` and set the port in which ESP32 is connected
	 
	 Note that, if you are using a bridge, probably you need to download some driver:
	 
	 - [CP210x](https://www.silabs.com/products/development-tools/software/usb-to-uart-bridge-vcp-drivers)
	 - [FTDI](https://www.ftdichip.com/Drivers/VCP.htm)
	 
	 [This](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/establish-serial-connection.html) provides a  guide on how establish serial connection between ESP32 and PC. 

4. Build code and flash the ESP32

		make all && make flash

5. See logs

		make monitor

# Configuration file

File `/main/Kconfig.projbuild` contains two differnt menu:

- SPIFFS (SPI Flash File System)

	It contains some important information about the SPIFFS partion:
	
	- SPIFFS Base address
	- SPIFFS Size
	- SPIFFS Logical block size
	- SPIFFS Logical page size
	
	[SPIFFS](https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/storage/spiffs.html) is a file system that supports wear leveling, file system consistency checks and more.

- Configurations

	It contains different variables, e.g:

	- ESP32_ID: ID of the ESP32
	- WIFI_SSID: SSID of WiFi (network name)
	- WIFI_PASS: WiFi password (WPA or WPA2)
	- BROKER_ADDR: IP of the MQTT broker
	- BROKER_PSW: password of the MQTT broker
	- BROKER_PORT: port of the MQTT broker
	- CHANNEL: channel in which ESP32 will sniff PROBE REQUEST
	- SNIFFING_TIME: time of sniffing
	- etc...

#### Configuration of variables 

In order to configure the variables mentioned above, open terminal inside the project folder and run
	
	make menuconfig 
	
1. Select the menu you want to modify
2. Edit variables as you like

#### Add customzied menu

You can also add different menus with different variables:

1. Open Kconfig.projbuild
2. Start menu with: *menu "menu name"*
3. Add the variables you need
4. End menu with: *endmenu*

# Components

- SPIFFS

	You need to create a [partition table](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/partition-tables.html)
	
	[SPIFFS components](https://github.com/espressif/esp-idf/tree/master/components/spiffs)

- ESP32 MQTT

	Has been used ESP32 MQTT Library
	
	[MQTT documentation](https://github.com/espressif/esp-mqtt/tree/c5ff6dd05fd357803f419916aa98ad7dd0f8e535)
	
- MD5

	Hash function used on the packets in order to get a unique identifier for each packet packet

# Resources

- Official [esp-idf git repo](https://github.com/espressif/esp-idf) to see some examples and information about the used data structure.
- Latest Documentation version for [Espressif IoT Development Framework](https://docs.espressif.com/projects/esp-idf/en/latest/).
- [ESP32 forum](https://esp32.com/) to ask questions and find community resources.
- [ESP32-IDF Documentation](http://esp32.info/docs/esp_idf/html/index.html) for browseable modules, namespaces, data structures and files.

Check also this demonstration [video](https://youtu.be/NMywky9Ts_w) to see how ESP32 works.

###### Seneca
> Longum iter est per praecepta, breve et efficax per exempla
