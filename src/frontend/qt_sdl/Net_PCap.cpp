/*
    Copyright 2016-2024 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <string.h>
#include <pcap/pcap.h>
#include "Net.h"
#include "Config.h"
#include "Platform.h"
#include "main.h"

#ifdef __WIN32__
	#include <iphlpapi.h>
#else
	#include <sys/types.h>
	#include <ifaddrs.h>
	#include <netinet/in.h>
        #ifdef __linux__
            #include <linux/if_packet.h>
        #else
            #include <net/if.h>
            #include <net/if_dl.h>
        #endif
#endif

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

// welp
#ifndef PCAP_OPENFLAG_PROMISCUOUS
#define PCAP_OPENFLAG_PROMISCUOUS 1
#endif


#define DECL_PCAP_FUNC(ret, name, args, args2) \
    typedef ret (*type_##name) args; \
    type_##name ptr_##name = nullptr; \
    ret name args { return ptr_##name args2; }

DECL_PCAP_FUNC(int, pcap_findalldevs, (pcap_if_t** alldevs, char* errbuf), (alldevs,errbuf))
DECL_PCAP_FUNC(void, pcap_freealldevs, (pcap_if_t* alldevs), (alldevs))
DECL_PCAP_FUNC(pcap_t*, pcap_open_live, (const char* src, int snaplen, int flags, int readtimeout, char* errbuf), (src,snaplen,flags,readtimeout,errbuf))
DECL_PCAP_FUNC(void, pcap_close, (pcap_t* dev), (dev))
DECL_PCAP_FUNC(int, pcap_setnonblock, (pcap_t* dev, int nonblock, char* errbuf), (dev,nonblock,errbuf))
DECL_PCAP_FUNC(int, pcap_sendpacket, (pcap_t* dev, const u_char* data, int len), (dev,data,len))
DECL_PCAP_FUNC(int, pcap_dispatch, (pcap_t* dev, int num, pcap_handler callback, u_char* data), (dev,num,callback,data))
DECL_PCAP_FUNC(const u_char*, pcap_next, (pcap_t* dev, struct pcap_pkthdr* hdr), (dev,hdr))


namespace Net_PCap
{

const char* PCapLibNames[] =
{
#ifdef __WIN32__
    // TODO: name for npcap in non-WinPCap mode
    "wpcap.dll",
#elif defined(__APPLE__)
    "libpcap.A.dylib",
    "libpcap.dylib",
#else
    // Linux lib names
    "libpcap.so.1",
    "libpcap.so",
#endif
    nullptr
};

AdapterData* Adapters = nullptr;
int NumAdapters = 0;

Platform::DynamicLibrary* PCapLib = nullptr;
pcap_t* PCapAdapter = nullptr;
AdapterData* PCapAdapterData;


#define LOAD_PCAP_FUNC(sym) \
    ptr_##sym = (type_##sym)DynamicLibrary_LoadFunction(lib, #sym); \
    if (!ptr_##sym) return false;

bool TryLoadPCap(Platform::DynamicLibrary *lib)
{
    LOAD_PCAP_FUNC(pcap_findalldevs)
    LOAD_PCAP_FUNC(pcap_freealldevs)
    LOAD_PCAP_FUNC(pcap_open_live)
    LOAD_PCAP_FUNC(pcap_close)
    LOAD_PCAP_FUNC(pcap_setnonblock)
    LOAD_PCAP_FUNC(pcap_sendpacket)
    LOAD_PCAP_FUNC(pcap_dispatch)
    LOAD_PCAP_FUNC(pcap_next)

    return true;
}

bool InitAdapterList()
{
    NumAdapters = 0;

    // TODO: how to deal with cases where an adapter is unplugged or changes config??
    if (!PCapLib)
    {
        PCapLib = nullptr;
        PCapAdapter = nullptr;

        for (int i = 0; PCapLibNames[i]; i++)
        {
            Platform::DynamicLibrary* lib = Platform::DynamicLibrary_Load(PCapLibNames[i]);
            if (!lib) continue;

            if (!TryLoadPCap(lib))
            {
                Platform::DynamicLibrary_Unload(lib);
                continue;
            }

            Log(LogLevel::Info, "PCap: lib %s, init successful\n", PCapLibNames[i]);
            PCapLib = lib;
            break;
        }

        if (PCapLib == nullptr)
        {
            Log(LogLevel::Error, "PCap: init failed\n");
            return false;
        }
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    int ret;

    pcap_if_t* alldevs;
    ret = pcap_findalldevs(&alldevs, errbuf);
    if (ret < 0 || alldevs == nullptr)
    {
        Log(LogLevel::Warn, "PCap: no devices available\n");
        return false;
    }

    pcap_if_t* dev = alldevs;
    while (dev) { NumAdapters++; dev = dev->next; }

    Adapters = new AdapterData[NumAdapters];
    memset(Adapters, 0, sizeof(AdapterData)*NumAdapters);

    AdapterData* adata = &Adapters[0];
    dev = alldevs;
    while (dev)
    {
        strncpy(adata->DeviceName, dev->name, 127);
        adata->DeviceName[127] = '\0';

#ifndef __WIN32__
        strncpy(adata->FriendlyName, adata->DeviceName, 127);
        adata->FriendlyName[127] = '\0';
#endif // __WIN32__

        dev = dev->next;
        adata++;
    }

#ifdef __WIN32__

    ULONG bufsize = 16384;
    IP_ADAPTER_ADDRESSES* buf = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, bufsize);
    ULONG uret = GetAdaptersAddresses(AF_INET, 0, nullptr, buf, &bufsize);
    if (uret == ERROR_BUFFER_OVERFLOW)
    {
        HeapFree(GetProcessHeap(), 0, buf);
        buf = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, bufsize);
        uret = GetAdaptersAddresses(AF_INET, 0, nullptr, buf, &bufsize);
    }
    if (uret != ERROR_SUCCESS)
    {
        Log(LogLevel::Error, "GetAdaptersAddresses() shat itself: %08X\n", uret);
        return false;
    }

    for (int i = 0; i < NumAdapters; i++)
    {
        adata = &Adapters[i];
        IP_ADAPTER_ADDRESSES* addr = buf;
        while (addr)
        {
            if (strcmp(addr->AdapterName, &adata->DeviceName[12]))
            {
                addr = addr->Next;
                continue;
            }

            WideCharToMultiByte(CP_UTF8, 0, addr->FriendlyName, 127, adata->FriendlyName, 127, nullptr, nullptr);
            adata->FriendlyName[127] = '\0';

            WideCharToMultiByte(CP_UTF8, 0, addr->Description, 127, adata->Description, 127, nullptr, nullptr);
            adata->Description[127] = '\0';

            if (addr->PhysicalAddressLength != 6)
            {
                Log(LogLevel::Warn, "weird MAC addr length %d for %s\n", addr->PhysicalAddressLength, addr->AdapterName);
            }
            else
                memcpy(adata->MAC, addr->PhysicalAddress, 6);

            IP_ADAPTER_UNICAST_ADDRESS* ipaddr = addr->FirstUnicastAddress;
            while (ipaddr)
            {
                SOCKADDR* sa = ipaddr->Address.lpSockaddr;
                if (sa->sa_family == AF_INET)
                {
                    struct in_addr sa4 = ((sockaddr_in*)sa)->sin_addr;
                    memcpy(adata->IP_v4, &sa4, 4);
                }

                ipaddr = ipaddr->Next;
            }

            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, buf);

#else

    struct ifaddrs* addrs;
    if (getifaddrs(&addrs) != 0)
    {
        Log(LogLevel::Error, "getifaddrs() shat itself :(\n");
        return false;
    }

    for (int i = 0; i < NumAdapters; i++)
    {
        adata = &Adapters[i];
        struct ifaddrs* curaddr = addrs;
        while (curaddr)
        {
            if (strcmp(curaddr->ifa_name, adata->DeviceName))
            {
                curaddr = curaddr->ifa_next;
                continue;
            }

            if (!curaddr->ifa_addr)
            {
                Log(LogLevel::Error, "Device (%s) does not have an address :/\n", curaddr->ifa_name);
                curaddr = curaddr->ifa_next;
                continue;
            }

            u16 af = curaddr->ifa_addr->sa_family;
            if (af == AF_INET)
            {
                struct sockaddr_in* sa = (sockaddr_in*)curaddr->ifa_addr;
                memcpy(adata->IP_v4, &sa->sin_addr, 4);
            }
#ifdef __linux__
            else if (af == AF_PACKET)
            {
                struct sockaddr_ll* sa = (sockaddr_ll*)curaddr->ifa_addr;
                if (sa->sll_halen != 6)
                    Log(LogLevel::Warn, "weird MAC length %d for %s\n", sa->sll_halen, curaddr->ifa_name);
                else
                    memcpy(adata->MAC, sa->sll_addr, 6);
            }
#else
            else if (af == AF_LINK)
            {
                struct sockaddr_dl* sa = (sockaddr_dl*)curaddr->ifa_addr;
                if (sa->sdl_alen != 6)
                    Log(LogLevel::Warn, "weird MAC length %d for %s\n", sa->sdl_alen, curaddr->ifa_name);
                else
                    memcpy(adata->MAC, LLADDR(sa), 6);
            }
#endif
            curaddr = curaddr->ifa_next;
        }
    }

    freeifaddrs(addrs);

#endif // __WIN32__

    pcap_freealldevs(alldevs);
    return true;
}

bool Init()
{
    if (!PCapLib) PCapAdapter = nullptr;
    if (PCapAdapter) pcap_close(PCapAdapter);

    InitAdapterList();

    // open pcap device
    Config::Table cfg = Config::GetGlobalTable();
    std::string devicename = cfg.GetString("LAN.Device");
    PCapAdapterData = &Adapters[0];
    for (int i = 0; i < NumAdapters; i++)
    {
        if (!strncmp(Adapters[i].DeviceName, devicename.c_str(), 128))
            PCapAdapterData = &Adapters[i];
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    PCapAdapter = pcap_open_live(PCapAdapterData->DeviceName, 2048, PCAP_OPENFLAG_PROMISCUOUS, 1, errbuf);
    if (!PCapAdapter)
    {
        Log(LogLevel::Error, "PCap: failed to open adapter %s\n", errbuf);
        return false;
    }

    if (pcap_setnonblock(PCapAdapter, 1, errbuf) < 0)
    {
        Log(LogLevel::Error, "PCap: failed to set nonblocking mode\n");
        pcap_close(PCapAdapter); PCapAdapter = nullptr;
        return false;
    }

    return true;
}

void DeInit()
{
    if (PCapLib)
    {
        if (PCapAdapter)
        {
            pcap_close(PCapAdapter);
            PCapAdapter = nullptr;
        }

        Platform::DynamicLibrary_Unload(PCapLib);
        PCapLib = nullptr;
    }
}


void RXCallback(u_char* userdata, const struct pcap_pkthdr* header, const u_char* data)
{
    Net::RXEnqueue(data, header->len);
}

int SendPacket(u8* data, int len)
{
    if (PCapAdapter == nullptr)
        return 0;

    if (len > 2048)
    {
        Log(LogLevel::Error, "Net_SendPacket: error: packet too long (%d)\n", len);
        return 0;
    }

    pcap_sendpacket(PCapAdapter, data, len);
    // TODO: check success
    return len;
}

void RecvCheck()
{
    if (PCapAdapter == nullptr)
        return;

    pcap_dispatch(PCapAdapter, 1, RXCallback, nullptr);
}

}
