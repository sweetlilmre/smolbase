I want to build a template firmware for a device called the Small TV Pro. I've already built one firmware for it, which you can find in the following directory D:\source\SmolTV-Pro\docs.
The intention with this template firmware is to build just enough for other people to build their own firmware on top of that. So, what that would include is:

1. the base firmware with a captive portal that will appear on first run once the user is joined to a network that captive portal will no longer appear
2. To support this, we'll need a high-performance local web server based on the Psychic HTTP server
3. We will need littleFS support in order to store static assets to serve for the captive portal and for the settings interface
4. Once the user has joined a network and they browse to the device on the network, then the configuration interface will be displayed that will be a served HTTP web page where you are able to configure various aspects of the device
5. When the device starts up and connects to the network successfully, it will make an NTP request in order to be able to obtain the accurate time. The user will then offset that time by choosing a time zone in the configuration settings
6. When the device is in AP mode, on the screen will be displayed the information for the user to connect to the AP in order to configure the Wi-Fi details
7. Once connected to the Wi-Fi, that screen will disappear and a stock screen will appear showing the IP address that the device has obtained

I want to build a modular and well-architected system that is as efficient as possible. I do not want to copy the existing source code in the SmolTV-Pro folder, but that can be as a reference, or some of that can be as a reference for the type of system that I would like to build. So, this is a clean from scratch build, not utilizing again that implementation, but doing it in the most speed and memory optimal way possible with very, very clean, readable, and extensible code.

Obtain what information you need from the original project in order to scaffold what I have requested up and get it into a building state. Utilize the hardware information to get the correct pinout for the screen. You can copy the partitions file to get the flash layout and then gain inspiration from the configuration and portal files

Build out the minimum viable product