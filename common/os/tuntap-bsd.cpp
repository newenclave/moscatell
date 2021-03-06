#include "../tuntap.h"

#if defined( __FreeBSD__) || defined( __OpenBSD__) ||  defined(__APPLE__)

#define UNIX_CODE

#include <iostream>

#include <netinet/in.h>
#include <net/if_tun.h>
#include <netinet/ip.h>


#include <netinet/in.h>
#include <net/if_var.h>
#include <netinet/in_var.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <stdlib.h>

#include "../utilities.h"

#include "posix-utils.h"

namespace msctl { namespace common {

namespace {

    namespace ba = boost::asio;

    using utilities::decorators::quote;

    void throw_errno( const std::string &name )
    {
        utilities::throw_errno( name );
    }

    using fd_keeper = utilities::fd_keeper;

    int opentuntap( const char *dev, std::string &resname )
    {
        if( dev && *dev ) {
            std::ostringstream oss;
            resname = dev;
            oss << "/dev/" << dev;
            return open( oss.str( ).c_str( ), O_RDWR );
        } else {
            for( int i=0; i<100; ++i ) {
                std::ostringstream oss;
                std::ostringstream devname;
                devname << "tun" << i;
                oss << "/dev/" << devname.str( );
                int fd = open( oss.str( ).c_str( ), O_RDWR );
                if( (fd >= 0) || (errno == ENOENT) ) {
                    resname = devname.str( );
                    return fd;
                }
            }
        }
        return -1;
    }


    int set_v4_params( native_handle /*device*/,
                       const char *devname,
                       std::uint32_t ip0,
                       std::uint32_t otherip,
                       std::uint32_t mask )
    {
        struct in_aliasreq addreq;
        fd_keeper s;

        s.fd_ = socket( AF_INET, SOCK_DGRAM, 0);

        if( s.fd_ < 0 ) {
            return -1;
        }

        memset( &addreq, 0, sizeof(addreq) );
        strncpy( addreq.ifra_name, devname, IFNAMSIZ );

        addreq.ifra_addr.sin_family         = AF_INET;
        addreq.ifra_addr.sin_addr.s_addr    = htonl(ip0);

        addreq.ifra_dstaddr.sin_family      = AF_INET;
        addreq.ifra_dstaddr.sin_addr.s_addr = htonl(otherip);

        addreq.ifra_mask.sin_family         = AF_INET;
        addreq.ifra_mask.sin_addr.s_addr    = htonl(mask);

        if (ioctl( s.fd_, SIOCSIFPHYADDR, &addreq) < 0 ) {
            close( s.fd_ );
            std::perror( "SIOCSIFPHYADDR" );
            return -1;
        }

        if (ioctl( s, SIOCSIFNETMASK, mask ) < 0 ) {
            close( s.fd_ );
            std::perror( "SIOCSIFPHYADDR" );
            return -1;
        }

        close( s.fd_ );
        return 0;
    }

}

    src_dest_v4 extract_ip_v4( const char *data, size_t len )
    {
        if( extract_family( data, len ) == 4 ) {
            auto hdr = reinterpret_cast<const ip *>(data);
            return std::make_pair( hdr->ip_src.s_addr,
                                   hdr->ip_dst.s_addr );
        }

        return src_dest_v4( );
    }

    int extract_family( const char *data, size_t len )
    {
        auto hdr = reinterpret_cast<const ip *>(data);
        if( len < sizeof(*hdr) ) {
            return -1;
        }
        return hdr->ip_v;
    }

    void close_handle( native_handle hdl )
    {
        ::close( hdl );
    }


    int device_up( const std::string &name )
    {
        return 0;
    }

    device_info open_tun( const std::string &hint_name )
    {
        device_info res;
        std::string name;
        auto hdl = opentuntap( hint_name.c_str( ), name );
        if( hdl == common::TUN_HANDLE_INVALID_VALUE ) {
            throw_errno( "open_tun." );
        }

        res.assign_name( name );
        res.assign( hdl );

        return std::move( res );
    }

    int del_tun( const std::string &name )
    {
        std::ostringstream cmd;
        cmd << "ifconfig " << name << " destroy";
        return system( cmd.str( ).c_str( ) );
    }

    addres_mask_v4 get_v4( const std::string &dev )
    {
        struct ifreq ifr;
        memset( &ifr, 0, sizeof(ifr) );

        strncpy( ifr.ifr_name, dev.c_str( ), IFNAMSIZ );

        fd_keeper s(socket(AF_INET, SOCK_DGRAM, 0));

        if( s < 0 )  {
            return addres_mask_v4( );
        }

        if( ioctl( s, SIOCGIFADDR, static_cast<void *>(&ifr) ) < 0 ) {
            return addres_mask_v4( );
        }
        sockaddr_in * sa = reinterpret_cast<sockaddr_in *>(&ifr.ifr_addr);
        auto addr = ba::ip::address_v4(ntohl(sa->sin_addr.s_addr));

        if( ioctl( s, SIOCGIFNETMASK, static_cast<void *>(&ifr) ) < 0 ) {
            return addres_mask_v4( );
        }
        sa = reinterpret_cast<sockaddr_in *>(&ifr.ifr_addr);
        auto mask = ba::ip::address_v4(ntohl(sa->sin_addr.s_addr));

        return std::make_pair( addr, mask );
    }


    void setup_device( native_handle device,
                       const std::string &name,
                       const std::string &ip,
                       const std::string &otherip,
                       const std::string &mask )
    {
        using av4  = boost::asio::ip::address_v4;
        av4 sip    = av4::from_string( ip );
        av4 sother = av4::from_string( otherip );
        av4 smask  = av4::from_string( mask );

        std::ostringstream oss;

        if( !sother.to_ulong( ) ) {
            sother = av4( sip.to_ulong( ) & smask.to_ulong( ) );
        }

        oss << "ifconfig " << quote(name, '"')
            << " " << ip << " " << sother.to_string( )
            << " netmask " << mask;

        system( oss.str( ).c_str( ) );

//        auto v4get = get_v4( name.c_str( ) );

//        std::cerr << v4get.first << " " << v4get.second << "\n";

        return;

        set_v4_params( device, name.c_str( ),
                       sip.to_ulong( ),
                       sother.to_ulong( ),
                       smask.to_ulong( ) );
    }

} }
#endif
