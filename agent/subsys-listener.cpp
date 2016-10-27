#include <mutex>

#include "subsys-listener.h"

#include "vtrc-server/vtrc-listener.h"
#include "vtrc-server/vtrc-listener-tcp.h"
#include "vtrc-server/vtrc-listener-local.h"

#include "vtrc-system.h"

#include "protocol/tuntap.pb.h"

#include "vtrc-common/vtrc-closure-holder.h"
#include "vtrc-common/vtrc-rpc-service-wrapper.h"
#include "vtrc-common/vtrc-stub-wrapper.h"
#include "vtrc-common/vtrc-mutex-typedefs.h"
#include "vtrc-server/vtrc-channels.h"

#include "common/tuntap.h"
#include "common/utilities.h"

#define LOG(lev) log_(lev, "listener")
#define LOGINF   LOG(logger_impl::level::info)
#define LOGDBG   LOG(logger_impl::level::debug)
#define LOGERR   LOG(logger_impl::level::error)
#define LOGWRN   LOG(logger_impl::level::warning)
namespace msctl { namespace agent {

namespace {

    namespace vcomm = vtrc::common;
    namespace vserv = vtrc::server;
    namespace ba    = boost::asio;
    using     utilities::decorators::quote;

    struct listener_info {
        vserv::listener_sptr point;
        std::string          device;

        listener_info( ) = default;

        listener_info( vserv::listener_sptr p, const std::string &d )
            :point(p)
            ,device(d)
        { }

        listener_info &operator = ( const listener_info &other )
        {
            point  = other.point;
            device = other.device;
            return *this;
        }
    };

    using listeners_map  = std::map<std::string, listener_info>;
    using server_stub    = rpc::tuntap::client_instance_Stub;
    using server_wrapper = vcomm::stub_wrapper<server_stub,
                                               vcomm::rpc_channel>;
    using server_wrapper_sptr = std::shared_ptr<server_wrapper>;


    /// works with routes
    /// one device -> many clients
    class server_transport: public common::tuntap_transport {

        std::map<std::uint32_t, server_wrapper_sptr> points_;

    public:

        using route_map = std::map<ba::ip::address, server_wrapper>;
        using parent_type = common::tuntap_transport;
        using empty_callback = std::function<void ()>;

        using route_table = std::map<std::uint32_t, server_wrapper_sptr>;

        server_transport( ba::io_service &ios )
            :parent_type(ios, 2048, parent_type::OPT_DISPATCH_READ)
        { }

        void on_read( const char *data, size_t length ) override
        {
            rpc::tuntap::push_req req;
            req.set_value( data, length );
#ifdef _WIN32
#else
            auto srcdst = common::extract_ip_v4( data, length );
            if( srcdst.second ) {
                auto f = points_.find( srcdst.second );
                if( f != points_.end( ) ) {
                    f->second->call_request( &server_stub::push, &req );
                }
            }
#endif

        }

        using shared_type = std::shared_ptr<server_transport>;

        void add_client_impl( vcomm::connection_iface_wptr clnt,
                              std::uint32_t ip )
        {
            using vserv::channels::unicast::create_event_channel;

            auto clntptr = clnt.lock( );
            if( !clntptr ) {
                return;
            }

            auto svc = std::make_shared<server_wrapper>
                                (create_event_channel(clntptr), true );

            points_[ip] = svc;
            svc->channel( )->set_flag( vcomm::rpc_channel::DISABLE_WAIT );

        }

        void add_client( vcomm::connection_iface *clnt, std::uint32_t ip )
        {
            auto wclnt = clnt->weak_from_this( );
            dispatch( [this, wclnt, ip]( ) {
                add_client_impl( wclnt, ip );
            } );
        }

        void del_client_impl( vcomm::connection_iface *clnt )
        {
//                for( auto &p: points_ ) {
//                    if( p.secong )
//                    points_.erase( id );
//                }
        }

        void del_client( vcomm::connection_iface *clnt  )
        {
            dispatch( [this, clnt ]( ) {
                del_client_impl( clnt );
            } );
        }

        static
        shared_type create( application *app, const std::string &device )
        {
            using std::make_shared;
            auto dev  = common::open_tun( device, false );
            if( dev < 0 ) {
                return shared_type( );
            }
//                if( common::device_up( device ) < 0) {
//                    return shared_type( );
//                }

            auto inst = make_shared<server_transport>
                                            ( app->get_io_service( ) );
            inst->get_stream( ).assign( dev );
            return inst;
        }

    };

    struct device_info {
        std::string                   name;
        server_transport::shared_type transport;
    };

    using device_info_sptr = std::shared_ptr<device_info>;

    using remote_map = std::map<std::uintptr_t, device_info_sptr>;
    using device_map = std::map<std::string,    device_info_sptr>;

    class svc_impl: public rpc::tuntap::server_instance {

        application                  *app_;
        vcomm::connection_iface      *client_;
        device_info_sptr              device_;

    public:

        using parent_type = rpc::tuntap::server_instance;

        svc_impl( application *app,
                  vcomm::connection_iface_wptr client,
                  device_info_sptr device )
            :app_(app)
            ,client_(client.lock( ).get( ))
            ,device_(device)
        {
//            auto &log_(app->log( ));
//            LOGINF << "Create service for " << client.lock( )->name( );
        }

        ~svc_impl( )
        {
            //device_->transport->del_client( client_ );
        }

        static std::string name( )
        {
            return parent_type::descriptor( )->full_name( );
        }

        void register_me( ::google::protobuf::RpcController* /*controller*/,
                          const ::msctl::rpc::tuntap::register_req* request,
                          ::msctl::rpc::tuntap::register_res* response,
                          ::google::protobuf::Closure* done) override
        {
            vcomm::closure_holder done_holder( done );

        }

        void push( ::google::protobuf::RpcController*   /*controller*/,
                   const ::msctl::rpc::tuntap::push_req* request,
                   ::msctl::rpc::tuntap::push_res*      /*response*/,
                   ::google::protobuf::Closure* done) override
        {
            vcomm::closure_holder done_holder( done );

            auto dev = reinterpret_cast<server_transport *>
                                                    (client_->user_data( ));
            if( dev ) {
                dev->write_post_notify( request->value( ),
                    [ ](const boost::system::error_code &err)
                    {
                        if( err ) {
                            //std::cout << "" << err.message( ) << "\n";
                        }
                    } );
            } else {
//                    auto hex = utilities::bin2hex( request->value( ) );
//                    LOGDBG << hex;
            }
        }
    };

    application::service_wrapper_sptr create_service(
                                  application *app,
                                  vcomm::connection_iface_wptr cl,
                                  device_info_sptr dev )
    {
        auto inst = std::make_shared<svc_impl>( app, cl, dev );
        return app->wrap_service( cl, inst );
    }

}
    struct listener::impl {

        application     *app_;
        listener        *parent_;
        logger_impl     &log_;

        listeners_map    points_;
        std::mutex       points_lock_;

        remote_map          remote_;
        device_map          devices_;
        vtrc::shared_mutex  remote_lock_;

        impl( logger_impl &log )
            :log_(log)
        { }

        void add_server_point( vcomm::connection_iface *c,
                               const listener::server_create_info &dev )
        {
            auto id = reinterpret_cast<std::uintptr_t>(c);
            vtrc::upgradable_lock lck(remote_lock_);
            auto f  =  devices_.find( dev.device );
            device_info_sptr info;
            if( f != devices_.end( ) ) {
                /// ok there is one device

                info = f->second;
                vtrc::upgrade_to_unique ulck(lck);
                remote_[id] = info;

            } else {
                /// ok there is no device

                info = std::make_shared<device_info>( );
                info->name      = dev.device;
                info->transport = server_transport::create( app_, dev.device );
                if( info->transport ) {
                    info->transport->start_read( );
                    vtrc::upgrade_to_unique ulck(lck);
                    devices_[dev.device] = remote_[id] = info;
                } else {
                    LOGERR << "Failed to open device: " << errno;
                }
            }
        }

        void del_server_point( vcomm::connection_iface *c )
        {
            auto id = reinterpret_cast<std::uintptr_t>(c);
            vtrc::upgradable_lock lck(remote_lock_);
            auto f = remote_.find( id);
            if( f != remote_.end( ) ) {
                f->second->transport->del_client( c ); /// remove from transport
                vtrc::upgrade_to_unique ulck(lck);
                remote_.erase( f );                    /// remove from endpoints
            }
        }

        void on_start( const listener::server_create_info &p )
        {
            LOGINF << "Point " << quote(p.point) << " was started.";
                      ;
        }

        void on_stop( const listener::server_create_info &p )
        {
            LOGINF << "Point " << quote(p.point) << " was stopped.";
                      ;
        }

        void on_accept_failed( const VTRC_SYSTEM::error_code &err,
                               const listener::server_create_info &inf )
        {
            LOGERR << "Accept failed on listener: " << quote(inf.point)
                   << "' assigned to device " << quote(inf.device) << "; "
                   << "Error: " << err.value( )
                   << " (" << err.message( ) << ")"
                      ;
        }

        void on_new_connection( vcomm::connection_iface *c,
                                const listener::server_create_info &dev )
        {
            LOGINF << "Add connection: " << quote(c->name( ))
                   << " for device " << quote(dev.device);
            add_server_point( c, dev );
            parent_->get_on_new_connection( )( c, dev.device );
        }

        void on_stop_connection( vcomm::connection_iface *c,
                                 const listener::server_create_info & /*dev*/ )
        {
            LOGINF << "Remove connection: " << quote(c->name( ));
            del_server_point( c );
            parent_->get_on_stop_connection( )( c );
        }

        bool add( const listener::server_create_info &serv_info, bool s )
        {
            using namespace vserv::listeners;

            auto &point(serv_info.point);
            auto &dev(serv_info.device);

            auto inf = utilities::get_endpoint_info( point );
            vserv::listener_sptr res;

            if( inf.is_local( ) ) {
                res = local::create( *app_, inf.addpess );
            } else if( inf.is_ip( ) ) {
                res = tcp::create( *app_, inf.addpess, inf.service, true );
            } else {
                LOGERR << "Failed to add endpoint '"
                       << point << "'; Bad format";
            }

            if( res ) {

                LOGINF << "Adding "      << quote(point)
                       << " for device " << quote(dev);

                res->on_start_connect(
                    [this, serv_info](  ) {
                        this->on_start( serv_info );
                    } );

                res->on_stop_connect (
                    [this, serv_info](  ) {
                        this->on_stop( serv_info );
                    } );

                res->on_accept_failed_connect(
                    [this, serv_info]( const VTRC_SYSTEM::error_code &err ) {
                        this->on_accept_failed( err, serv_info );
                    } );

                res->on_new_connection_connect(
                    [this, serv_info]( vcomm::connection_iface *c ) {
                        this->on_new_connection( c, serv_info );
                    } );

                res->on_stop_connection_connect(
                    [this, serv_info]( vcomm::connection_iface *c ) {
                        this->on_stop_connection( c, serv_info );
                    } );

                if( s ) {
                    res->start( );
                }

                std::lock_guard<std::mutex> lck(points_lock_);
                points_[point] = listener_info( res, dev );
                return true;
            }

            return false;
        }

        void start_all( )
        {
            std::lock_guard<std::mutex> lck(points_lock_);
            for( auto &p: points_ ) {
                LOGINF << "Starting " << quote(p.first) << "...";
                p.second.point->start( );
                LOGINF << "Ok.";
            }
        }

        void reg_creator( )
        {
            using res_type = application::service_wrapper_sptr;
            auto creator = [this]( application *app,
                                   vcomm::connection_iface_wptr cl ) -> res_type
            {
                auto id = reinterpret_cast<std::uintptr_t>(cl.lock( ).get( ));

                vtrc::shared_lock lck(remote_lock_);
                auto f  =  remote_.find( id );
                return f == remote_.end( )
                        ? res_type( )
                        : create_service( app, cl, f->second );
            };

            app_->register_service_factory( svc_impl::name( ), creator );
        }

        void unreg_creator( )
        {
            app_->unregister_service_factory( svc_impl::name( ) );
        }

    };

    listener::listener( application *app )
        :impl_(new impl(app->log( )))
    {
        impl_->app_ = app;
        impl_->parent_ = this;
    }

    void listener::init( )
    { }

    void listener::start( )
    {
        impl_->start_all( );
        impl_->reg_creator( );
        impl_->LOGINF << "Started.";
    }

    void listener::stop( )
    {
        impl_->unreg_creator( );
        impl_->LOGINF << "Stopped.";
    }

    std::shared_ptr<listener> listener::create( application *app )
    {
        return std::make_shared<listener>( app );
    }

    bool listener::add_server( const server_create_info &inf, bool start )
    {
        return impl_->add( inf, start );
    }

}}

