/*
 * SocketServer.cpp
 *
 *  Created on: 25 Mar 2017
 *      Author: David
 */

#include "ecv.h"
#undef yield
#undef array

extern "C"
{
	#include "user_interface.h"     // for struct rst_info
	#include "lwip/init.h"			// for version info
	#include "lwip/stats.h"			// for stats_display()

#if LWIP_VERSION_MAJOR == 2
	#include "lwip/apps/mdns.h"
	#include "lwip/apps/netbiosns.h"
#else
	#include "lwip/app/netbios.h"	// for NetBIOS support
#endif
}

#include <cstdarg>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include "SocketServer.h"
#include "Config.h"
#include "PooledStrings.h"
#include "HSPI.h"

#include "include/MessageFormats.h"
#include "Connection.h"
#include "Listener.h"
#include "Misc.h"

#include <cstring>
#include <algorithm>

extern "C"
{
	#include "esp_task_wdt.h"
}

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_partition.h"

#include "rom/ets_sys.h"
#include "driver/gpio.h"

#include "esp8266/spi.h"
#include "esp8266/gpio.h"

const gpio_num_t ONBOARD_LED = GPIO_NUM_2;			// GPIO 2
const bool ONBOARD_LED_ON = false;					// active low
const uint32_t ONBOARD_LED_BLINK_INTERVAL = 500;	// ms
const uint32_t TransferReadyTimeout = 10;			// how many milliseconds we allow for the Duet to set TransferReady low after the end of a transaction, before we assume that we missed seeing it

#if LWIP_VERSION_MAJOR == 2
const char * const MdnsProtocolNames[3] = { "HTTP", "FTP", "Telnet" };
const char * const MdnsServiceStrings[3] = { "_http", "_ftp", "_telnet" };
const char * const MdnsTxtRecords[2] = { "product=DuetWiFi", "version=" VERSION_MAIN };
const unsigned int MdnsTtl = 10 * 60;			// same value as on the Duet 0.6/0.8.5
#else
# include <ESP8266mDNS.h>
#endif

#define array _ecv_array

const uint32_t MaxConnectTime = 40 * 1000;		// how long we wait for WiFi to connect in milliseconds
const uint32_t StatusReportMillis = 200;

const int DefaultWiFiChannel = 6;

// Global data
char currentSsid[SsidLength + 1];
char webHostName[HostNameLength + 1] = "Duet-WiFi";

DNSServer dns;

static const char* lastError = nullptr;
static const char* prevLastError = nullptr;
static uint32_t whenLastTransactionFinished = 0;
static bool connectErrorChanged = false;
static bool transferReadyChanged = false;

static char lastConnectError[100];

static WiFiState currentState = WiFiState::idle,
				prevCurrentState = WiFiState::disabled,
				lastReportedState = WiFiState::disabled;
static uint32_t lastBlinkTime = 0;

ADC_MODE(ADC_VCC);          // need this for the ESP.getVcc() call to work

static HSPIClass hspi;
static uint32_t connectStartTime;
static uint32_t lastStatusReportTime;
static uint32_t transferBuffer[NumDwords(MaxDataLength + 1)];

static int ssidIdx = -1;
static const esp_partition_t* ssids = nullptr;

typedef enum {
    STATION_IDLE = 0,
    STATION_CONNECTING,
    STATION_WRONG_PASSWORD,
    STATION_NO_AP_FOUND,
    STATION_CONNECT_FAIL,
    STATION_GOT_IP
} station_status_t;

static station_status_t wifiStatus;
static station_status_t wifi_station_get_connect_status(void) { return wifiStatus; };

// Look up a SSID in our remembered network list, return pointer to it if found
int RetrieveSsidData(const char *ssid, WirelessConfigurationData& data)
{
	for (size_t i = 1; i <= MaxRememberedNetworks; ++i)
	{
		WirelessConfigurationData d;
		esp_partition_read(ssids, i * sizeof(WirelessConfigurationData), &d, sizeof(WirelessConfigurationData));
		if (strncmp(ssid, d.ssid, sizeof(d.ssid)) == 0)
		{
			memcpy(&data, &d, sizeof(data));
			return i;
		}
	}
	return -1;
}

int FindEmptySsidEntry()
{
	for (size_t i = 1; i <= MaxRememberedNetworks; ++i)
	{
		WirelessConfigurationData d;
		esp_partition_read(ssids, i * sizeof(WirelessConfigurationData), &d, sizeof(WirelessConfigurationData));
		if (d.ssid[0] == 0xFF)
		{
			return i;
		}
	}
	return -1;
}

// Check socket number in range, returning true if yes. Otherwise, set lastError and return false;
bool ValidSocketNumber(uint8_t num)
{
	if (num < MaxConnections)
	{
		return true;
	}
	lastError = "socket number out of range";
	return false;
}

// Reset to default settings
void FactoryReset()
{
	esp_partition_erase_range(ssids, 0, ssids->size);
}

static void wifi_evt_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		wifiStatus = STATION_CONNECTING;
        esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        switch (disconnected->reason) {
			case WIFI_REASON_AUTH_FAIL:
				wifiStatus = STATION_WRONG_PASSWORD;
				break;
			case WIFI_REASON_NO_AP_FOUND:
				wifiStatus = STATION_NO_AP_FOUND;
				break;
			case WIFI_REASON_ASSOC_LEAVE:
				wifiStatus = STATION_IDLE;
				break;
			default:
				wifiStatus = STATION_CONNECT_FAIL;
				break;
		}
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, webHostName));
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		wifiStatus = STATION_GOT_IP;
	}
}

// Try to connect using the specified SSID and password
void ConnectToAccessPoint(const WirelessConfigurationData& apData, bool isRetry)
pre(currentState == NetworkState::idle)
{
	SafeStrncpy(currentSsid, apData.ssid, ARRAY_SIZE(currentSsid));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );

	wifi_config_t wifi_config;
	memcpy(wifi_config.sta.ssid, apData.ssid, sizeof(wifi_config.sta.ssid));
	memcpy(wifi_config.sta.password, apData.password, sizeof(wifi_config.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );

	if (apData.ip != 0) {
		tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
		tcpip_adapter_ip_info_t ip_info;
		ip_info.ip.addr = apData.ip;
		ip_info.gw.addr = apData.gateway;
		ip_info.netmask.addr = apData.netmask;
		ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
	}

	debugPrintf("Trying to connect to ssid \"%s\" with password \"%s\"\n", apData.ssid, apData.password);
	ESP_ERROR_CHECK(esp_wifi_start() );

//	WiFi.setAutoReconnect(false);								// auto reconnect NEVER works in our configuration so disable it, it just wastes time
	WiFi.setAutoReconnect(true);
#if NO_WIFI_SLEEP
	wifi_set_sleep_type(NONE_SLEEP_T);
#else
	wifi_set_sleep_type(MODEM_SLEEP_T);
#endif

	if (isRetry)
	{
		currentState = WiFiState::reconnecting;
	}
	else
	{
		currentState = WiFiState::connecting;
		connectStartTime = millis();
	}
}

void ConnectPoll()
{
	// The Arduino WiFi.status() call is fairly useless here because it discards too much information, so use the SDK API call instead
	const uint8_t status = wifi_station_get_connect_status();
	const char *error = nullptr;
	bool retry = false;

	switch (currentState)
	{
	case WiFiState::connecting:
	case WiFiState::reconnecting:
		// We are trying to connect or reconnect, so check for success or failure
		switch (status)
		{
		case STATION_IDLE:
			error = "Unexpected WiFi state 'idle'";
			break;

		case STATION_CONNECTING:
			if (millis() - connectStartTime >= MaxConnectTime)
			{
				error = "Timed out";
			}
			break;

		case STATION_WRONG_PASSWORD:
			error = "Wrong password";
			break;

		case STATION_NO_AP_FOUND:
			error = "Didn't find access point";
			retry = (currentState == WiFiState::reconnecting);
			break;

		case STATION_CONNECT_FAIL:
			error = "Failed";
			retry = (currentState == WiFiState::reconnecting);
			break;

		case STATION_GOT_IP:
			if (currentState == WiFiState::reconnecting)
			{
				lastError = "Reconnect succeeded";
			}
			else
			{
// #if LWIP_VERSION_MAJOR == 2
// 				mdns_resp_netif_settings_changed(netif_list);	// STA is on first interface
// #else
// 				MDNS.begin(webHostName);
// #endif
			}

			debugPrint("Connected to AP\n");
			currentState = WiFiState::connected;
			gpio_set_level(ONBOARD_LED, ONBOARD_LED_ON);
			break;

		default:
			error = "Unknown WiFi state";
			break;
		}

		if (error != nullptr)
		{
			strcpy(lastConnectError, error);
			SafeStrncat(lastConnectError, " while trying to connect to ", ARRAY_SIZE(lastConnectError));
			SafeStrncat(lastConnectError, currentSsid, ARRAY_SIZE(lastConnectError));
			lastError = lastConnectError;
			connectErrorChanged = true;
			debugPrint("Failed to connect to AP\n");

			if (!retry)
			{
				esp_wifi_stop();
				currentState = WiFiState::idle;
				gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
			}
		}
		break;

	case WiFiState::connected:
		if (status != STATION_GOT_IP)
		{
			// We have just lost the connection
			connectStartTime = millis();						// start the auto reconnect timer

			switch (status)
			{
			case STATION_CONNECTING:							// auto reconnecting
				error = "auto reconnecting";
				currentState = WiFiState::autoReconnecting;
				break;

			case STATION_IDLE:
				error = "state 'idle'";
				retry = true;
				break;

			case STATION_WRONG_PASSWORD:
				error = "state 'wrong password'";
				currentState = WiFiState::idle;
				gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
				break;

			case STATION_NO_AP_FOUND:
				error = "state 'no AP found'";
				retry = true;
				break;

			case STATION_CONNECT_FAIL:
				error = "state 'fail'";
				retry = true;
				break;

			default:
				error = "unknown WiFi state";
				currentState = WiFiState::idle;
				gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
				break;
			}

			strcpy(lastConnectError, "Lost connection, ");
			SafeStrncat(lastConnectError, error, ARRAY_SIZE(lastConnectError));
			lastError = lastConnectError;
			connectErrorChanged = true;
			debugPrint("Lost connection to AP\n");
			break;
		}
		break;

	case WiFiState::autoReconnecting:
		if (status == STATION_GOT_IP)
		{
			lastError = "Auto reconnect succeeded";
			currentState = WiFiState::connected;
		}
		else if (status != STATION_CONNECTING && lastError == nullptr)
		{
			lastError = "Auto reconnect failed, trying manual reconnect";
			connectStartTime = millis();						// start the manual reconnect timer
			retry = true;
		}
		else if (millis() - connectStartTime >= MaxConnectTime)
		{
			lastError = "Timed out trying to auto-reconnect";
			retry = true;
		}
		break;

	default:
		break;
	}

	if (retry)
	{
		WirelessConfigurationData d;
		esp_partition_read(ssids, ssidIdx * sizeof(WirelessConfigurationData), &d, sizeof(WirelessConfigurationData));
		ConnectToAccessPoint(d, true);
	}
}

void StartClient(const char * array ssid)
pre(currentState == WiFiState::idle)
{
	WirelessConfigurationData wp;

	if (ssid == nullptr || ssid[0] == 0)
	{
		// Auto scan for strongest known network, then try to connect to it
		const int8_t num_ssids = WiFi.scanNetworks(false, true);
		if (num_ssids < 0)
		{
			lastError = "network scan failed";
			currentState = WiFiState::idle;
			gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
			return;
		}

		// Find the strongest network that we know about
		int8_t strongestNetwork = -1;
		for (int8_t i = 0; i < num_ssids; ++i)
		{
			debugPrintfAlways("found network %s\n", WiFi.SSID(i).c_str());
			if (strongestNetwork < 0 || WiFi.RSSI(i) > WiFi.RSSI(strongestNetwork))
			{
				if (RetrieveSsidData(WiFi.SSID(i).c_str(), wp) > 0)
				{
					strongestNetwork = i;
				}
			}
		}
		if (strongestNetwork < 0)
		{
			lastError = "no known networks found";
			return;
		}

		ssidIdx = strongestNetwork;
	}
	else
	{
		ssidIdx = RetrieveSsidData(ssid, wp);
		if (ssidIdx < 0)
		{
			lastError = "no data found for requested SSID";
			return;
		}
	}

	// ssidData contains the details of the strongest known access point
	ConnectToAccessPoint(wp, false);
}

bool CheckValidSSID(const char * array s)
{
	size_t len = 0;
	while (*s != 0)
	{
		if (*s < 0x20 || *s == 0x7F)
		{
			return false;					// bad character
		}
		++s;
		++len;
		if (len == SsidLength)
		{
			return false;					// ESP8266 core requires strlen(ssid) <= 31
		}
	}
	return len != 0;
}

bool CheckValidPassword(const char * array s)
{
	size_t len = 0;
	while (*s != 0)
	{
		if (*s < 0x20 || *s == 0x7F)
		{
			return false;					// bad character
		}
		++s;
		++len;
		if (len == PasswordLength)
		{
			return false;					// ESP8266 core requires strlen(password) <= 63
		}
	}
	return len == 0 || len >= 8;			// password must be empty or at least 8 characters (WPA2 restriction)
}

// Check that the access point data is valid
bool ValidApData(const WirelessConfigurationData &apData)
{
	// Check the IP address
	if (apData.ip == 0 || apData.ip == 0xFFFFFFFF)
	{
		return false;
	}

	// Check the channel. 0 means auto so it OK.
	if (apData.channel > 13)
	{
		return false;
	}

	return CheckValidSSID(apData.ssid) && CheckValidPassword(apData.password);
}

void StartAccessPoint()
{
	WirelessConfigurationData apData;
	esp_partition_read(ssids, 0, &apData, sizeof(WirelessConfigurationData));

	if (ValidApData(apData))
	{
		esp_err_t res = esp_wifi_set_mode(WIFI_MODE_AP);

		SafeStrncpy(currentSsid, apData.ssid, ARRAY_SIZE(currentSsid));
		if (res == ESP_OK)
		{
			if (res == ESP_OK)
			{
				wifi_config_t wifi_config;
				memcpy(wifi_config.ap.ssid, currentSsid, sizeof(wifi_config.ap.ssid));
				memcpy(wifi_config.ap.password, apData.password, sizeof(wifi_config.ap.password));
				wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
				wifi_config.ap.channel = (apData.channel == 0) ? DefaultWiFiChannel : apData.channel;

				res = esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);

				if (res == ESP_OK)
				{
					tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);

					tcpip_adapter_ip_info_t ip_info;
					ip_info.ip.addr = apData.ip;
					ip_info.gw.addr = apData.gateway;
					ip_info.netmask = IPADDR4_INIT_BYTES(255, 255, 255, 0);
					res = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);

					if (res == ESP_OK) {
						debugPrintf("Starting AP %s with password \"%s\"\n", currentSsid, apData.password);
						res = esp_wifi_start();
					}
				}

				if (res != ESP_OK)
				{
					debugPrintAlways("Failed to start AP\n");
				}
			}
			else
			{
				debugPrintAlways("Failed to set AP config\n");
			}
		}
		else
		{
			debugPrintAlways("Failed to set AP mode\n");
		}

		if (res == ESP_OK)
		{
			debugPrintAlways("AP started\n");
			dns.setErrorReplyCode(DNSReplyCode::NoError);
			if (!dns.start(53, "*", apData.ip))
			{
				lastError = "Failed to start DNS\n";
				debugPrintf("%s\n", lastError);
			}
			SafeStrncpy(currentSsid, apData.ssid, ARRAY_SIZE(currentSsid));
			currentState = WiFiState::runningAsAccessPoint;
			gpio_set_level(ONBOARD_LED, ONBOARD_LED_ON);
// #if LWIP_VERSION_MAJOR == 2
// 			mdns_resp_netif_settings_changed(netif_list->next);		// AP is on second interface
// #else
// 			MDNS.begin(webHostName);
// #endif
		}
		else
		{
			esp_wifi_stop();
			lastError = "Failed to start access point";
			debugPrintf("%s\n", lastError);
			currentState = WiFiState::idle;
			gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
		}
	}
	else
	{
		lastError = "invalid access point configuration";
		debugPrintf("%s\n", lastError);
		currentState = WiFiState::idle;
		gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
	}
}

static union
{
	MessageHeaderSamToEsp hdr;			// the actual header
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderIn;

static union
{
	MessageHeaderEspToSam hdr;
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderOut;

// #if LWIP_VERSION_MAJOR == 2

// void GetServiceTxtEntries(struct mdns_service *service, void *txt_userdata)
// {
// 	for (size_t i = 0; i < ARRAY_SIZE(MdnsTxtRecords); i++)
// 	{
// 		mdns_resp_add_service_txtitem(service, MdnsTxtRecords[i], strlen(MdnsTxtRecords[i]));
// 	}
// }

// // Rebuild the mDNS services
// void RebuildServices()
// {
// 	for (struct netif *item = netif_list; item != nullptr; item = item->next)
// 	{
// 		mdns_resp_remove_netif(item);
// 		mdns_resp_add_netif(item, webHostName, MdnsTtl);
// 		mdns_resp_add_service(item, "echo", "_echo", DNSSD_PROTO_TCP, 0, 0, nullptr, nullptr);

// 		for (size_t protocol = 0; protocol < 3; protocol++)
// 		{
// 			const uint16_t port = Listener::GetPortByProtocol(protocol);
// 			if (port != 0)
// 			{
// 				service_get_txt_fn_t txtFunc = (protocol == 0/*HttpProtocol*/) ? GetServiceTxtEntries : nullptr;
// 				mdns_resp_add_service(item, webHostName, MdnsServiceStrings[protocol], DNSSD_PROTO_TCP, port, MdnsTtl, txtFunc, nullptr);
// 			}
// 		}

// 		mdns_resp_netif_settings_changed(item);
// 	}
// }

// void RemoveMdnsServices()
// {
// 	for (struct netif *item = netif_list; item != nullptr; item = item->next)
// 	{
// 		mdns_resp_remove_netif(item);
// 	}
// }

// #else

// // Rebuild the MDNS server to advertise a single service
// void AdvertiseService(int service, uint16_t port)
// {
// 	static int currentService = -1;
// 	static const char * const serviceNames[] = { "http", "tcp", "ftp" };

// 	if (service != currentService)
// 	{
// 		currentService = service;
// 		MDNS.deleteServices();
// 		if (service >= 0 && service < (int)ARRAY_SIZE(serviceNames))
// 		{
// 			const char* serviceName = serviceNames[service];
// 			MDNS.addService(serviceName, "tcp", port);
// 			MDNS.addServiceTxt(serviceName, "tcp", "product", "DuetWiFi");
// 			MDNS.addServiceTxt(serviceName, "tcp", "version", firmwareVersion);
// 		}
// 	}
// }

// // Rebuild the mDNS services
// void RebuildServices()
// {
// 	if (currentState == WiFiState::connected)		// MDNS server only works in station mode
// 	{
// 		// Unfortunately the official ESP8266 mDNS library only reports one service.
// 		// I (chrishamm) tried to use the old mDNS responder, which is also capable of sending
// 		// mDNS broadcasts, but the packets it generates are broken and thus not of use.
// 		for (int service = 0; service < 3; ++service)
// 		{
// 			const uint16_t port = Listener::GetPortByProtocol(service);
// 			if (port != 0)
// 			{
// 				AdvertiseService(service, port);
// 				return;
// 			}
// 		}

// 		AdvertiseService(-1, 0);		// no services to advertise
// 	}
// }

// #endif

// Send a response.
// 'response' is the number of byes of response if positive, or the error code if negative.
// Use only to respond to commands which don't include a data block, or when we don't want to read the data block.
void IRAM_ATTR SendResponse(int32_t response)
{
	(void)hspi.transfer32(response);
	if (response > 0)
	{
		hspi.transferDwords(transferBuffer, nullptr, NumDwords((size_t)response));
	}
}

// This is called when the SAM is asking to transfer data
void IRAM_ATTR ProcessRequest()
{
	// Set up our own headers
	messageHeaderIn.hdr.formatVersion = InvalidFormatVersion;
	messageHeaderOut.hdr.formatVersion = MyFormatVersion;
	messageHeaderOut.hdr.state = currentState;
	bool deferCommand = false;

	// Begin the transaction
	gpio_set_level(SamSSPin, 0);		// assert CS to SAM
	hspi.beginTransaction();

	// Exchange headers, except for the last dword which will contain our response
	hspi.transferDwords(messageHeaderOut.asDwords, messageHeaderIn.asDwords, headerDwords - 1);

	if (messageHeaderIn.hdr.formatVersion != MyFormatVersion)
	{
		SendResponse(ResponseBadRequestFormatVersion);
	}
	else if (messageHeaderIn.hdr.dataLength > MaxDataLength)
	{
		SendResponse(ResponseBadDataLength);
	}
	else
	{
		const size_t dataBufferAvailable = std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength);

		// See what command we have received and take appropriate action
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::nullCommand:					// no command being sent, SAM just wants the network status
			SendResponse(ResponseEmpty);
			break;

		case NetworkCommand::networkStartClient:			// connect to an access point
			if (currentState == WiFiState::idle)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				if (messageHeaderIn.hdr.dataLength != 0 && messageHeaderIn.hdr.dataLength <= SsidLength + 1)
				{
					hspi.transferDwords(nullptr, transferBuffer, NumDwords(messageHeaderIn.hdr.dataLength));
					reinterpret_cast<char *>(transferBuffer)[messageHeaderIn.hdr.dataLength] = 0;
				}
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			if (currentState == WiFiState::idle)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkFactoryReset:			// clear remembered list, reset factory defaults
			deferCommand = true;
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			deferCommand = true;
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			break;

		case NetworkCommand::networkGetStatus:				// get the network connection status
			{
				const bool runningAsAp = (currentState == WiFiState::runningAsAccessPoint);
				const bool runningAsStation = (currentState == WiFiState::connected);
				NetworkStatusResponse * const response = reinterpret_cast<NetworkStatusResponse*>(transferBuffer);
				response->freeHeap = esp_get_free_heap_size();

				switch (esp_reset_reason())
				{
				case ESP_RST_POWERON:
					response->resetReason = 0; // Power-on
					break;
				case ESP_RST_WDT:
					response->resetReason = 1; // Hardware watchdog
					break;
				case ESP_RST_PANIC:
					response->resetReason = 2; // Exception
					break;
				case ESP_RST_TASK_WDT:
				case ESP_RST_INT_WDT:
					response->resetReason = 3; // Software watchdog
					break;
				case ESP_RST_SW:
				case ESP_RST_FAST_SW:
					response->resetReason = 4; // Software-initiated reset
					break;
				case ESP_RST_DEEPSLEEP:
					response->resetReason = 5; // Wake from deep-sleep
					break;
				case ESP_RST_EXT:
					response->resetReason = 6; // External reset
					break;
				case ESP_RST_SDIO:
				case ESP_RST_UNKNOWN:
				case ESP_RST_BROWNOUT:
				default:
					response->resetReason = 99; // Out-of-range, translates to 'Unknown' in RRF
					break;
				}

				if (runningAsStation) {
					wifi_ap_record_t ap_info;
					esp_wifi_sta_get_ap_info(&ap_info);
					response->rssi = ap_info.rssi; 
					response->numClients = 0;
					esp_wifi_get_mac(WIFI_IF_STA, response->macAddress);
					tcpip_adapter_ip_info_t ip_info;
					tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
					response->ipAddress = ip_info.ip.addr;
				} else if (runningAsAp) {
					wifi_sta_list_t sta_list;
					esp_wifi_ap_get_sta_list(&sta_list);

					response->numClients = sta_list.num; 
					response->rssi = 0;
					esp_wifi_get_mac(WIFI_IF_AP, response->macAddress);
					tcpip_adapter_ip_info_t ip_info;
					tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
					response->ipAddress = ip_info.ip.addr;
				} else {
					response->ipAddress = 0;
				}

				response->flashSize = spi_flash_get_chip_size();
				response->sleepMode = (uint8_t)wifi_get_sleep_type() + 1;
				response->phyMode = (uint8_t)wifi_get_phy_mode();
				response->zero1 = 0;
				response->zero2 = 0;
				response->vcc = system_get_vdd33();
			    SafeStrncpy(response->versionText, firmwareVersion, sizeof(response->versionText));
			    SafeStrncpy(response->hostName, webHostName, sizeof(response->hostName));
			    SafeStrncpy(response->ssid, currentSsid, sizeof(response->ssid));
			    response->clockReg = REG(SPI_CLOCK(HSPI));
				SendResponse(sizeof(NetworkStatusResponse));
			}
			break;

		case NetworkCommand::networkAddSsid:				// add to our known access point list
		case NetworkCommand::networkConfigureAccessPoint:	// configure our own access point details
			if (messageHeaderIn.hdr.dataLength == sizeof(WirelessConfigurationData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(sizeof(WirelessConfigurationData)));
				const WirelessConfigurationData * const receivedClientData = reinterpret_cast<const WirelessConfigurationData *>(transferBuffer);
				int index;
				if (messageHeaderIn.hdr.command == NetworkCommand::networkConfigureAccessPoint)
				{
					index = 0;
				}
				else
				{
					WirelessConfigurationData d;
					index = RetrieveSsidData(receivedClientData->ssid, d);
					if (index < 0)
					{
						index = FindEmptySsidEntry();
					}
				}

				if (index >= 0)
				{
					esp_partition_write(ssids, index * sizeof(WirelessConfigurationData), receivedClientData, sizeof(WirelessConfigurationData));
				}
				else
				{
					lastError = "SSID table full";
				}
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkDeleteSsid:				// delete a network from our access point list
			if (messageHeaderIn.hdr.dataLength == SsidLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(SsidLength));

				WirelessConfigurationData d;
				int index = RetrieveSsidData(reinterpret_cast<char*>(transferBuffer), d);

				if (index >= 0)
				{
					WirelessConfigurationData localSsidData;
					memset(&localSsidData, 0xFF, sizeof(localSsidData));

					esp_partition_write(ssids, index * sizeof(WirelessConfigurationData), &localSsidData, sizeof(WirelessConfigurationData));
				}
				else
				{
					lastError = "SSID not found";
				}
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkRetrieveSsidData:	// list the access points we know about, including our own access point details
			if (dataBufferAvailable < ReducedWirelessConfigurationDataSize)
			{
				SendResponse(ResponseBufferTooSmall);
			}
			else
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 0; i <= MaxRememberedNetworks && (i + 1) * ReducedWirelessConfigurationDataSize <= dataBufferAvailable; ++i)
				{

					WirelessConfigurationData tempData;
					esp_partition_read(ssids, i * sizeof(WirelessConfigurationData), &tempData, sizeof(WirelessConfigurationData));
					if (tempData.ssid[0] != 0xFF)
					{
						memcpy(p, &tempData, ReducedWirelessConfigurationDataSize);
						p += ReducedWirelessConfigurationDataSize;
					}
					else if (i == 0)
					{
						memset(p, 0, ReducedWirelessConfigurationDataSize);
						p += ReducedWirelessConfigurationDataSize;
					}
				}
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				SendResponse(numBytes);
			}
			break;

		case NetworkCommand::networkListSsids_deprecated:	// list the access points we know about, plus our own access point details
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 0; i <= MaxRememberedNetworks; ++i)
				{
					WirelessConfigurationData tempData;
					esp_partition_read(ssids, i * sizeof(WirelessConfigurationData), &tempData, sizeof(WirelessConfigurationData));
					if (tempData.ssid[0] != 0xFF)
					{
						for (size_t j = 0; j < SsidLength && tempData.ssid[j] != 0; ++j)
						{
							*p++ = tempData.ssid[j];
						}
						*p++ = '\n';
					}
					else if (i == 0)
					{
						// Include an empty entry for our own access point SSID
						*p++ = '\n';
					}
				}
				*p++ = 0;
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				if (numBytes <= dataBufferAvailable)
				{
					SendResponse(numBytes);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
			}
			break;

		case NetworkCommand::networkSetHostName:			// set the host name
			if (messageHeaderIn.hdr.dataLength == HostNameLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(HostNameLength));
				memcpy(webHostName, transferBuffer, HostNameLength);
				webHostName[HostNameLength] = 0;			// ensure null terminator
#if LWIP_VERSION_MAJOR == 2
				netbiosns_set_name(webHostName);
#endif
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkGetLastError:
			if (lastError == nullptr)
			{
				SendResponse(0);
			}
			else
			{
				const size_t len = strlen(lastError) + 1;
				if (dataBufferAvailable >= len)
				{
					strcpy(reinterpret_cast<char*>(transferBuffer), lastError);		// copy to 32-bit aligned buffer
					SendResponse(len);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
				lastError = nullptr;
			}
			lastReportedState = currentState;
			break;

		case NetworkCommand::networkListen:				// listen for incoming connections
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				const bool ok = Listener::Listen(lcData.remoteIp, lcData.port, lcData.protocol, lcData.maxConnections);
				if (ok)
				{
					if (lcData.protocol < 3)			// if it's FTP, HTTP or Telnet protocol
					{
						// RebuildServices();				// update the MDNS services
					}
					debugPrintf("%sListening on port %u\n", (lcData.maxConnections == 0) ? "Stopped " : "", lcData.port);
				}
				else
				{
					lastError = "Listen failed";
					debugPrint("Listen failed\n");
				}
			}
			break;

#if 0	// We don't use the following command, instead we use networkListen with maxConnections = 0
		case NetworkCommand::unused_networkStopListening:
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				Listener::StopListening(lcData.port);
				RebuildServices();						// update the MDNS services
				debugPrintf("Stopped listening on port %u\n", lcData.port);
			}
			break;
#endif

		case NetworkCommand::connAbort:					// terminate a socket rudely
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Terminate(true);
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connClose:					// close a socket gracefully
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Close();
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connRead:					// read data from a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t amount = conn.Read(reinterpret_cast<uint8_t *>(transferBuffer), std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength));
				messageHeaderIn.hdr.param32 = hspi.transfer32(amount);
				hspi.transferDwords(transferBuffer, nullptr, NumDwords(amount));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connWrite:					// write data to a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t requestedlength = messageHeaderIn.hdr.dataLength;
				const size_t acceptedLength = std::min<size_t>(conn.CanWrite(), std::min<size_t>(requestedlength, MaxDataLength));
				const bool closeAfterSending = (acceptedLength == requestedlength) && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagCloseAfterWrite) != 0;
				const bool push = (acceptedLength == requestedlength) && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagPush) != 0;
				messageHeaderIn.hdr.param32 = hspi.transfer32(acceptedLength);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(acceptedLength));
				const size_t written = conn.Write(reinterpret_cast<uint8_t *>(transferBuffer), acceptedLength, push, closeAfterSending);
				if (written != acceptedLength)
				{
					lastError = "incomplete write";
				}
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connGetStatus:				// get the status of a socket, and summary status for all sockets
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(sizeof(ConnStatusResponse));
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				ConnStatusResponse resp;
				conn.GetStatus(resp);
				Connection::GetSummarySocketStatus(resp.connectedSockets, resp.otherEndClosedSockets);
				hspi.transferDwords(reinterpret_cast<const uint32_t *>(&resp), nullptr, NumDwords(sizeof(resp)));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::diagnostics:					// print some debug info over the UART line
			SendResponse(ResponseEmpty);
			deferCommand = true;							// we need to send the diagnostics after we have sent the response, so the SAM is ready to receive them
			break;

		case NetworkCommand::networkSetTxPower:
			{
				const uint8_t txPower = messageHeaderIn.hdr.flags;
				if (txPower <= 82)
				{
					system_phy_set_max_tpw(txPower);
					SendResponse(ResponseEmpty);
				}
				else
				{
					SendResponse(ResponseBadParameter);
				}
			}
			break;

		case NetworkCommand::networkSetClockControl:
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			deferCommand = true;
			break;

		case NetworkCommand::connCreate:					// create a connection
			// Not implemented yet
		default:
			SendResponse(ResponseUnknownCommand);
			break;
		}
	}

	gpio_set_level(SamSSPin, 1);			// de-assert CS to SAM to end the transaction and tell SAM the transfer is complete
	hspi.endTransaction();

	// If we deferred the command until after sending the response (e.g. because it may take some time to execute), complete it now
	if (deferCommand)
	{
		// The following functions must set up lastError if an error occurs
		lastError = nullptr;								// assume no error
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::networkStartClient:			// connect to an access point
			if (messageHeaderIn.hdr.dataLength == 0 || reinterpret_cast<const char*>(transferBuffer)[0] == 0)
			{
				StartClient(nullptr);						// connect to strongest known access point
			}
			else
			{
				StartClient(reinterpret_cast<const char*>(transferBuffer));		// connect to specified access point
			}
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			StartAccessPoint();
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			Connection::TerminateAll();						// terminate all connections
			Listener::StopListening(0);						// stop listening on all ports
			// RebuildServices();								// remove the MDNS services
			switch (currentState)
			{
			case WiFiState::connected:
			case WiFiState::connecting:
			case WiFiState::reconnecting:
// #if LWIP_VERSION_MAJOR == 2
// 				RemoveMdnsServices();
// #endif
// #if LWIP_VERSION_MAJOR == 1
// 				MDNS.deleteServices();
// #endif
				delay(20);									// try to give lwip time to recover from stopping everything
				esp_wifi_disconnect();
				break;

			case WiFiState::runningAsAccessPoint:
				dns.stop();
				delay(20);									// try to give lwip time to recover from stopping everything
				esp_wifi_disconnect();
				break;

			default:
				break;
			}
			delay(100);
			currentState = WiFiState::idle;
			gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);
			break;

		case NetworkCommand::networkFactoryReset:			// clear remembered list, reset factory defaults
			FactoryReset();
			break;

		case NetworkCommand::diagnostics:
			Connection::ReportConnections();
			delay(20);										// give the Duet main processor time to digest that
			stats_display();
			break;

		case NetworkCommand::networkSetClockControl:
			hspi.setClockDivider(messageHeaderIn.hdr.param32);
			break;

		default:
			lastError = "bad deferred command";
			break;
		}
	}
}

void IRAM_ATTR TransferReadyIsr(void *)
{
	transferReadyChanged = true;
}


void setup()
{
	gpio_reset_pin(ONBOARD_LED);
	gpio_set_direction(ONBOARD_LED, GPIO_MODE_OUTPUT);
	gpio_set_level(ONBOARD_LED, !ONBOARD_LED_ON);

    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifiStatus = STATION_IDLE;

	ssids = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
													 ESP_PARTITION_SUBTYPE_DATA_NVS, 
													 "ssids");

	const size_t eepromSizeNeeded = (MaxRememberedNetworks + 1) * sizeof(WirelessConfigurationData);
	assert(eepromSizeNeeded < ssids->size);

	// Set up the SPI subsystem
	gpio_reset_pin(SamTfrReadyPin);
	gpio_set_direction(SamTfrReadyPin, GPIO_MODE_INPUT);

	gpio_reset_pin(EspReqTransferPin);
	gpio_set_direction(EspReqTransferPin, GPIO_MODE_OUTPUT);
	gpio_set_level(EspReqTransferPin, 0);

	gpio_reset_pin(SamSSPin);
	gpio_set_direction(SamSSPin, GPIO_MODE_OUTPUT);
	gpio_set_level(SamSSPin, 1);

	// Set up the fast SPI channel
	hspi.InitMaster(SPI_MODE1, defaultClockControl, true);

    Connection::Init();
//     Listener::Init();
#if LWIP_VERSION_MAJOR == 2
//     mdns_resp_init();
	for (struct netif *item = netif_list; item != nullptr; item = item->next)
	{
// 		mdns_resp_add_netif(item, webHostName, MdnsTtl);
	}
//     netbiosns_init();
#else
//     netbios_init();
#endif
	lastError = nullptr;
	debugPrint("Init completed\n");

	gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
	gpio_isr_handler_add(SamTfrReadyPin, TransferReadyIsr, nullptr);
	gpio_set_intr_type(SamTfrReadyPin, GPIO_INTR_POSEDGE);
	whenLastTransactionFinished = millis();
	lastStatusReportTime = millis();
	gpio_set_level(EspReqTransferPin, 1);					// tell the SAM we are ready to receive a command
}

void loop()
{
	gpio_set_level(EspReqTransferPin, 1);					// tell the SAM we are ready to receive a command
	esp_task_wdt_reset();									// kick the watchdog

	if (	(lastError != prevLastError || connectErrorChanged || currentState != prevCurrentState)
		|| ((lastError != nullptr || currentState != lastReportedState) && millis() - lastStatusReportTime > StatusReportMillis)
		)
	{
	 	ets_delay_us(2);									// make sure the pin stays high for long enough for the SAM to see it
		gpio_set_level(EspReqTransferPin, 0);			// force a low to high transition to signal that an error message is available
		ets_delay_us(2);									// make sure it is low enough to create an interrupt when it goes high
		gpio_set_level(EspReqTransferPin, 1);			// tell the SAM we are ready to receive a command
		prevLastError = lastError;
		prevCurrentState = currentState;
		connectErrorChanged = false;
		lastStatusReportTime = millis();
	}

	// // See whether there is a request from the SAM.
	// // Duet WiFi 1.04 and earlier have hardware to ensure that TransferReady goes low when a transaction starts.
	// // Duet 3 Mini doesn't, so we need to see TransferReady go low and then high again. In case that happens so fast that we dn't get the interrupt, we have a timeout.
	if (gpio_get_level(SamTfrReadyPin) == 1 && (transferReadyChanged || millis() - whenLastTransactionFinished > TransferReadyTimeout))
	{
		transferReadyChanged = false;
		ProcessRequest();
		whenLastTransactionFinished = millis();
	}

	ConnectPoll();
	// Connection::PollOne();

	if (currentState == WiFiState::runningAsAccessPoint)
	{
	// 	dns.processNextRequest();
	}
	else if (	(currentState == WiFiState::autoReconnecting ||
				 currentState == WiFiState::connecting ||
				 currentState == WiFiState::reconnecting) &&
				(millis() - lastBlinkTime > ONBOARD_LED_BLINK_INTERVAL))
	{
		lastBlinkTime = millis();
		gpio_set_level(ONBOARD_LED, !gpio_get_level(ONBOARD_LED));
	}
}

// End
