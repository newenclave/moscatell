
#ifndef SUBSYS_listener2_H
#define SUBSYS_listener2_H

#include "application.h"

#include "srpc/common/observers/simple.h"
#include "srpc/common/observers/define.h"

#include "common/create-params.h"

namespace msctl { namespace agent {

    class listener2: public common::subsys_iface {

        struct          impl;
        friend struct   impl;
        impl           *impl_;

    public:

        struct server_create_info {
            std::string                     point;
            std::string                     device;
            utilities::address_v4_poll      addr_poll;
            bool                            mcast       = true;
            bool                            bcast       = false;
            bool                            udp         = true;
            common::create_parameters       common;
        };

        listener2( application *app );

        static std::shared_ptr<listener2> create( application *app );
        static const char *name( ) 
        {
            return "listener2";
        }

        bool add_server( const server_create_info &inf, bool start );

    private:

        void init( )  override;
        void start( ) override;
        void stop( )  override;
    };

}}

#endif // SUBSYS_listener2_H

    
