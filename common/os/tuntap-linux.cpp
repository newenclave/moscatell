#include "../tuntap.h"

#if defined(__linux__)

#include <string.h>
#include <unistd.h>
#include <net/if.h>

#include <linux/if_tun.h>
//#include <linux/ipv6.h>
#include <linux/ip.h>

#define TUNTAP_DEVICE_NAME "/dev/net/tun"

#include <sys/ioctl.h>
#include <fcntl.h>
#include <netinet/in.h>

#include "boost/asio/ip/address_v4.hpp"
#include "boost/asio/ip/address_v6.hpp"


namespace msctl { namespace common {

namespace {

    namespace ba = boost::asio;

    void throw_errno( const std::string &name )
    {
        std::error_code ec(errno, std::system_category( ));
        throw std::runtime_error( name + " " + ec.message( ) );
    }

    struct fd_keeper {
        int fd_ = -1;

        fd_keeper( )
        { }

        explicit fd_keeper( int fd )
            :fd_(fd)
        { }

        ~fd_keeper( )
        {
            if( fd_ != -1 ) {
                close( fd_ );
            }
        }

        operator int ( )
        {
            return fd_;
        }

        int release( )
        {
            int tmp = fd_;
            fd_ = -1;
            return tmp;
        }
    };

    int set_dev_up( const char *devname )
    {
        struct ifreq ifr;
        struct sockaddr_in addr;
        fd_keeper s;

        memset( &ifr, 0, sizeof(ifr) );
        memset( &addr, 0, sizeof(addr) );
        strncpy(ifr.ifr_name, devname, IFNAMSIZ);

        addr.sin_family = AF_INET;
        s.fd_ = socket(addr.sin_family, SOCK_DGRAM, 0);

        if( s.fd_ < 0 ) {
            return -1;
        }

        ifr.ifr_flags = IFF_UP | IFF_RUNNING;

        if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0 ) {
            return -1;
        }
        return 0;
    }

    int opentuntap( const char *dev, int flags, bool persis )
    {
        const char *clonedev = TUNTAP_DEVICE_NAME;

        struct ifreq ifr;
        int fd = -1;

        if( (fd = open(clonedev , O_RDWR)) < 0 ) {
            return fd;
        }

        memset(&ifr, 0, sizeof(ifr));

        ifr.ifr_flags = flags;

        if (*dev) {
            strncpy(ifr.ifr_name, dev, IFNAMSIZ);
        }

        if( ioctl( fd, TUNSETIFF, static_cast<void *>(&ifr) ) < 0 ) {
            close( fd );
            return -1;
        }

        flags = fcntl( fd, F_GETFL, 0 );
        if( flags < 0 ) {
            close( fd );
            return -1;
        }

        if( fcntl( fd, F_SETFL, flags | O_NONBLOCK ) < 0) {
            close( fd );
            return -1;
        }

        if( persis ) {
            ioctl( fd, TUNSETPERSIST, 1 );
        }

        return fd;
    }

    int set_v4_param( const char *devname,
                      unsigned long code,
                      std::uint32_t param )
    {
        struct ifreq ifr;
        struct sockaddr_in addr;
        fd_keeper s;

        memset( &ifr, 0, sizeof(ifr) );
        memset( &addr, 0, sizeof(addr) );
        strncpy(ifr.ifr_name, devname, IFNAMSIZ);

        addr.sin_family = AF_INET;
        s.fd_ = socket(addr.sin_family, SOCK_DGRAM, 0);

        if( s.fd_ < 0 ) {
            return -1;
        }

        addr.sin_addr.s_addr = param;
        ifr.ifr_addr = *reinterpret_cast<sockaddr *>(&addr);

        if (ioctl(s, code, (caddr_t) &ifr) < 0 ) {
            return -1;
        }
        return 0;
    }

    int setip_v4_mask( const char *devname, const char *ip_addr )
    {
        boost::system::error_code ec;
        auto ip = ba::ip::address_v4::from_string( ip_addr, ec );
        if( ec ) {
            errno = ec.value( );
            return -1;
        }
        return set_v4_param( devname, SIOCSIFNETMASK, htonl(ip.to_ulong( )) );
    }

    int setip_v4_addr( const char *devname, const char *ip_addr )
    {
        boost::system::error_code ec;
        auto ip = ba::ip::address_v4::from_string( ip_addr, ec );
        if( ec ) {
            errno = ec.value( );
            return -1;
        }
        return set_v4_param( devname, SIOCSIFADDR, htonl(ip.to_ulong( )) );
    }
}

    src_dest_v4 extract_ip_v4( const char *data, size_t len )
    {
        if( extract_family( data, len ) == 4 ) {
            auto hdr = reinterpret_cast<const iphdr *>(data);
            return std::make_pair( hdr->saddr, hdr->daddr );
        }
        return src_dest_v4( );
    }

    int extract_family( const char *data, size_t len )
    {
        auto hdr = reinterpret_cast<const iphdr *>(data);
        if( len < sizeof(*hdr) ) {
            return -1;
        }
        return hdr->version;
    }

    int device_up( const std::string &name )
    {
        return set_dev_up( name.c_str( ) );
    }

    device_info open_tun( const std::string &hint_name )
    {
        device_info res;

        res.handle = opentuntap( hint_name.c_str( ),
                                 IFF_TUN | IFF_NO_PI, true );
        if( res.handle == common::TUN_HANDLE_INVALID_VALUE ) {
            throw_errno( "open_tun." );
        }

        return std::move( res );
    }

    void close_handle( native_handle hdl )
    {
        ::close( hdl );
    }

    int del_tun( const std::string &name )
    {
        const char *clonedev = TUNTAP_DEVICE_NAME;

        struct ifreq ifr;
        int fd = -1;

        if( (fd = open(clonedev , O_RDWR)) < 0 ) {
            return fd;
        }

        memset(&ifr, 0, sizeof(ifr));

        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

        strncpy(ifr.ifr_name, name.c_str( ), IFNAMSIZ);

        if( ioctl( fd, TUNSETIFF, static_cast<void *>(&ifr) ) < 0 ) {
            close( fd );
            return -1;
        }

        ioctl( fd, TUNSETPERSIST, 0 );

        return fd;
    }

    void setup_device( native_handle /*device*/,
                       const std::string &name,
                       const std::string &ip,
                       const std::string & /*otherip*/,
                       const std::string &mask )
    {
        int res = setip_v4_addr( name.c_str( ), ip.c_str( ) );
        if( res < 0 ) {
            throw_errno( "ioctl(SIOCSIFADDR)" );
        }
        res = setip_v4_mask( name.c_str( ), mask.c_str( ) );
        if( res < 0 ) {
            throw_errno( "ioctl(SIOCSIFNETMASK)" );
        }
        device_up( name );
    }

}}

#endif