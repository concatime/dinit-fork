#ifndef SERVICE_LISTENER_HPP
#define SERVICE_LISTENER_HPP

#include "service_constants.hpp"

class service_record;

// Interface for listening to services
class service_listener {
public:
	// An event occurred on the service being observed.
	// Listeners must not be added or removed during event notification.
	virtual void service_event(service_record *service,
	                           service_event_t event) noexcept = 0;
};

#endif /* SERVICE_LISTENER_HPP */
