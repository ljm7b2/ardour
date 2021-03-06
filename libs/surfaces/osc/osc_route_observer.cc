/*
    Copyright (C) 2009 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "boost/lambda/lambda.hpp"

#include "pbd/control_math.h"
#include <glibmm.h>


#include "ardour/session.h"
#include "ardour/track.h"
#include "ardour/monitor_control.h"
#include "ardour/dB.h"
#include "ardour/meter.h"
#include "ardour/solo_isolate_control.h"

#include "osc.h"
#include "osc_route_observer.h"

#include "pbd/i18n.h"

using namespace std;
using namespace PBD;
using namespace ARDOUR;
using namespace ArdourSurface;

OSCRouteObserver::OSCRouteObserver (OSC& o, uint32_t ss, ArdourSurface::OSC::OSCSurface* su)
	: _osc (o)
	,ssid (ss)
	,sur (su)
	,_last_gain (-1.0)
	,_last_trim (-1.0)
	,_init (true)
	,_expand (2048)
{
	addr = lo_address_new_from_url (sur->remote_url.c_str());
	gainmode = sur->gainmode;
	feedback = sur->feedback;
	in_line = feedback[2];
	uint32_t sid = sur->bank + ssid - 2;
	uint32_t not_ready = 0;
	if (sur->linkset) {
		not_ready = _osc.link_sets[sur->linkset].not_ready;
	}
	if (not_ready) {
		set_link_ready (not_ready);
	} else if (sid >= sur->strips.size ()) {
		// this _should_ only occure if the number of strips is less than banksize
		_strip = boost::shared_ptr<ARDOUR::Stripable>();
		clear_strip ();
	} else {
		_strip = sur->strips[sid];
		refresh_strip (_strip, true);
	}
	if (sur->expand_enable) {
		set_expand (sur->expand);
	} else {
		set_expand (0);
	}
}

OSCRouteObserver::~OSCRouteObserver ()
{
	_init = true;
	strip_connections.drop_connections ();

	lo_address_free (addr);
}

void
OSCRouteObserver::no_strip ()
{
	// This gets called on drop references
	_init = true;

	strip_connections.drop_connections ();
	/*
	 * The strip will sit idle at this point doing nothing until
	 * the surface has recalculated it's strip list and then calls
	 * refresh_strip. Otherwise refresh strip will get a strip address
	 * that does not exist... Crash
	 */
 }
	
void
OSCRouteObserver::refresh_strip (boost::shared_ptr<ARDOUR::Stripable> new_strip, bool force)
{
	_init = true;
	if (_tick_busy) {
		Glib::usleep(100); // let tick finish
	}
	_last_gain =-1.0;
	_last_trim =-1.0;

	send_select_status (ARDOUR::Properties::selected);

	if ((new_strip == _strip) && !force) {
		// no change don't send feedback
		_init = false;
		return;
	}
	strip_connections.drop_connections ();
	_strip = new_strip;
	if (!_strip) {
		// this strip is blank and should be cleared
		clear_strip ();
		return;
	}
	_strip->DropReferences.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::no_strip, this), OSC::instance());
	as = ARDOUR::Off;

	if (feedback[0]) { // buttons are separate feedback
		_strip->PropertyChanged.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::name_changed, this, boost::lambda::_1), OSC::instance());
		name_changed (ARDOUR::Properties::name);

		_strip->presentation_info().PropertyChanged.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::pi_changed, this, _1), OSC::instance());
		_osc.int_message_with_id ("/strip/hide", ssid, _strip->is_hidden (), in_line, addr);

		_strip->mute_control()->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_change_message, this, X_("/strip/mute"), _strip->mute_control()), OSC::instance());
		send_change_message ("/strip/mute", _strip->mute_control());

		_strip->solo_control()->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_change_message, this, X_("/strip/solo"), _strip->solo_control()), OSC::instance());
		send_change_message ("/strip/solo", _strip->solo_control());

		if (_strip->solo_isolate_control()) {
			_strip->solo_isolate_control()->Changed.connect (strip_connections, MISSING_INVALIDATOR, bind (&OSCRouteObserver::send_change_message, this, X_("/strip/solo_iso"), _strip->solo_isolate_control()), OSC::instance());
			send_change_message ("/strip/solo_iso", _strip->solo_isolate_control());
		}

		if (_strip->solo_safe_control()) {
			_strip->solo_safe_control()->Changed.connect (strip_connections, MISSING_INVALIDATOR, bind (&OSCRouteObserver::send_change_message, this, X_("/strip/solo_safe"), _strip->solo_safe_control()), OSC::instance());
			send_change_message ("/strip/solo_safe", _strip->solo_safe_control());
		}

		boost::shared_ptr<Track> track = boost::dynamic_pointer_cast<Track> (_strip);
		if (track) {
			track->monitoring_control()->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_monitor_status, this, track->monitoring_control()), OSC::instance());
			send_monitor_status (track->monitoring_control());
		}

		boost::shared_ptr<AutomationControl> rec_controllable = _strip->rec_enable_control ();
		if (rec_controllable) {
			rec_controllable->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_change_message, this, X_("/strip/recenable"), _strip->rec_enable_control()), OSC::instance());
			send_change_message ("/strip/recenable", _strip->rec_enable_control());
		}
		boost::shared_ptr<AutomationControl> recsafe_controllable = _strip->rec_safe_control ();
		if (rec_controllable) {
			recsafe_controllable->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_change_message, this, X_("/strip/record_safe"), _strip->rec_safe_control()), OSC::instance());
			send_change_message ("/strip/record_safe", _strip->rec_safe_control());
		}
		_strip->presentation_info().PropertyChanged.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_select_status, this, _1), OSC::instance());
		send_select_status (ARDOUR::Properties::selected);
	}

	if (feedback[1]) { // level controls
		boost::shared_ptr<GainControl> gain_cont = _strip->gain_control();
		gain_cont->alist()->automation_state_changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::gain_automation, this), OSC::instance());
		gain_cont->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_gain_message, this), OSC::instance());
		gain_automation ();

		boost::shared_ptr<Controllable> trim_controllable = boost::dynamic_pointer_cast<Controllable>(_strip->trim_control());
		if (trim_controllable) {
			trim_controllable->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_trim_message, this), OSC::instance());
			send_trim_message ();
		}

		boost::shared_ptr<Controllable> pan_controllable = boost::dynamic_pointer_cast<Controllable>(_strip->pan_azimuth_control());
		if (pan_controllable) {
			pan_controllable->Changed.connect (strip_connections, MISSING_INVALIDATOR, boost::bind (&OSCRouteObserver::send_change_message, this, X_("/strip/pan_stereo_position"), _strip->pan_azimuth_control()), OSC::instance());
			send_change_message ("/strip/pan_stereo_position", _strip->pan_azimuth_control());
		}
	}
	_init = false;
	tick();

}

void
OSCRouteObserver::set_expand (uint32_t expand)
{
	if (expand != _expand) {
		_expand = expand;
		if (expand == ssid) {
			_osc.float_message_with_id ("/strip/expand", ssid, 1.0, in_line, addr);
		} else {
			_osc.float_message_with_id ("/strip/expand", ssid, 0.0, in_line, addr);
		}
	}
}

void
OSCRouteObserver::set_link_ready (uint32_t not_ready)
{
	if (not_ready) {
		clear_strip ();
		switch (ssid) {
			case 1:
				_osc.text_message_with_id ("/strip/name", ssid, "Device", in_line, addr);
				break;
			case 2:
				_osc.text_message_with_id ("/strip/name", ssid, string_compose ("%1", not_ready), in_line, addr);
				break;
			case 3:
				_osc.text_message_with_id ("/strip/name", ssid, "Missing", in_line, addr);
				break;
			case 4:
				_osc.text_message_with_id ("/strip/name", ssid, "from", in_line, addr);
				break;
			case 5:
				_osc.text_message_with_id ("/strip/name", ssid, "Linkset", in_line, addr);
				break;
			default:
				break;
		}
	} else {
		refresh_strip (_strip, true);
	}
}

void
OSCRouteObserver::clear_strip ()
{
	_init = true;

	strip_connections.drop_connections ();

	// all strip buttons should be off and faders 0 and etc.
	_osc.float_message_with_id ("/strip/expand", ssid, 0, in_line, addr);
	if (feedback[0]) { // buttons are separate feedback
		_osc.text_message_with_id ("/strip/name", ssid, " ", in_line, addr);
		_osc.float_message_with_id ("/strip/mute", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/solo", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/recenable", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/record_safe", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/monitor_input", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/monitor_disk", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/gui_select", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/select", ssid, 0, in_line, addr);
	}
	if (feedback[1]) { // level controls
		if (gainmode) {
			_osc.float_message_with_id ("/strip/fader", ssid, 0, in_line, addr);
		} else {
			_osc.float_message_with_id ("/strip/gain", ssid, -193, in_line, addr);
		}
		_osc.float_message_with_id ("/strip/trimdB", ssid, 0, in_line, addr);
		_osc.float_message_with_id ("/strip/pan_stereo_position", ssid, 0.5, in_line, addr);
	}
	if (feedback[9]) {
		_osc.float_message_with_id ("/strip/signal", ssid, 0, in_line, addr);
	}
	if (feedback[7]) {
		if (gainmode) {
			_osc.float_message_with_id ("/strip/meter", ssid, 0, in_line, addr);
		} else {
			_osc.float_message_with_id ("/strip/meter", ssid, -193, in_line, addr);
		}
	}else if (feedback[8]) {
		_osc.float_message_with_id ("/strip/meter", ssid, 0, in_line, addr);
	}
}


void
OSCRouteObserver::tick ()
{
	if (_init) {
		return;
	}
	_tick_busy = true;
	if (feedback[7] || feedback[8] || feedback[9]) { // meters enabled
		// the only meter here is master
		float now_meter;
		if (_strip->peak_meter()) {
			now_meter = _strip->peak_meter()->meter_level(0, MeterMCP);
		} else {
			now_meter = -193;
		}
		if (now_meter < -120) now_meter = -193;
		if (_last_meter != now_meter) {
			if (feedback[7] || feedback[8]) {
				if (gainmode && feedback[7]) {
					_osc.float_message_with_id ("/strip/meter", ssid, ((now_meter + 94) / 100), in_line, addr);
				} else if ((!gainmode) && feedback[7]) {
					_osc.float_message_with_id ("/strip/meter", ssid, now_meter, in_line, addr);
				} else if (feedback[8]) {
					uint32_t ledlvl = (uint32_t)(((now_meter + 54) / 3.75)-1);
					uint16_t ledbits = ~(0xfff<<ledlvl);
					_osc.int_message_with_id ("/strip/meter", ssid, ledbits, in_line, addr);
				}
			}
			if (feedback[9]) {
				float signal;
				if (now_meter < -40) {
					signal = 0;
				} else {
					signal = 1;
				}
				_osc.float_message_with_id ("/strip/signal", ssid, signal, in_line, addr);
			}
		}
		_last_meter = now_meter;

	}
	if (feedback[1]) {
		if (gain_timeout) {
			if (gain_timeout == 1) {
				_osc.text_message_with_id ("/strip/name", ssid, _strip->name(), in_line, addr);
			}
			gain_timeout--;
		}
		if (as == ARDOUR::Play ||  as == ARDOUR::Touch) {
			if(_last_gain != _strip->gain_control()->get_value()) {
				_last_gain = _strip->gain_control()->get_value();
				send_gain_message ();
			}
		}
	}
	_tick_busy = false;
}

void
OSCRouteObserver::name_changed (const PBD::PropertyChange& what_changed)
{
	if (!what_changed.contains (ARDOUR::Properties::name)) {
	    return;
	}

	if (_strip) {
		_osc.text_message_with_id ("/strip/name", ssid, _strip->name(), in_line, addr);
	}
}

void
OSCRouteObserver::pi_changed (PBD::PropertyChange const& what_changed)
{
	_osc.int_message_with_id ("/strip/hide", ssid, _strip->is_hidden (), in_line, addr);
}

void
OSCRouteObserver::send_change_message (string path, boost::shared_ptr<Controllable> controllable)
{
	float val = controllable->get_value();
	_osc.float_message_with_id (path, ssid, (float) controllable->internal_to_interface (val), in_line, addr);
}

void
OSCRouteObserver::send_monitor_status (boost::shared_ptr<Controllable> controllable)
{
	int disk, input;
	float val = controllable->get_value();
	switch ((int) val) {
		case 1:
			disk = 0;
			input = 1;
			break;
		case 2:
			disk = 1;
			input = 0;
			break;
		case 3:
			disk = 1;
			input = 1;
			break;
		default:
			disk = 0;
			input = 0;
	}
	_osc.int_message_with_id ("/strip/monitor_input", ssid, input, in_line, addr);
	_osc.int_message_with_id ("/strip/monitor_disk", ssid, disk, in_line, addr);

}

void
OSCRouteObserver::send_trim_message ()
{
	if (_last_trim != _strip->trim_control()->get_value()) {
		_last_trim = _strip->trim_control()->get_value();
	} else {
		return;
	}
	_osc.float_message_with_id ("/strip/trimdB", ssid, (float) accurate_coefficient_to_dB (_last_trim), in_line, addr);
}

void
OSCRouteObserver::send_gain_message ()
{
	boost::shared_ptr<Controllable> controllable = _strip->gain_control();
	if (_last_gain != controllable->get_value()) {
		_last_gain = controllable->get_value();
	} else {
		return;
	}

	if (gainmode) {
		_osc.float_message_with_id ("/strip/fader", ssid, controllable->internal_to_interface (_last_gain), in_line, addr);
		if (gainmode == 1) {
			_osc.text_message_with_id ("/strip/name", ssid, string_compose ("%1%2%3", std::fixed, std::setprecision(2), accurate_coefficient_to_dB (controllable->get_value())), in_line, addr);
			gain_timeout = 8;
		}
	}
	if (!gainmode || gainmode == 2) {
		if (controllable->get_value() < 1e-15) {
			_osc.float_message_with_id ("/strip/gain", ssid, -200, in_line, addr);
		} else {
			_osc.float_message_with_id ("/strip/gain", ssid, accurate_coefficient_to_dB (_last_gain), in_line, addr);
		}
	}
}

void
OSCRouteObserver::gain_automation ()
{
	string path = "/strip/gain";
	if (gainmode) {
		path = "/strip/fader";
	}
	send_gain_message ();
	as = _strip->gain_control()->alist()->automation_state();
	string auto_name;
	float output = 0;
	switch (as) {
		case ARDOUR::Off:
			output = 0;
			auto_name = "Manual";
			break;
		case ARDOUR::Play:
			output = 1;
			auto_name = "Play";
			break;
		case ARDOUR::Write:
			output = 2;
			auto_name = "Write";
			break;
		case ARDOUR::Touch:
			output = 3;
			auto_name = "Touch";
			break;
		case ARDOUR::Latch:
			output = 4;
			auto_name = "Latch";
			break;
		default:
			break;
	}
	_osc.float_message_with_id (string_compose ("%1/automation", path), ssid, output, in_line, addr);
	_osc.text_message_with_id (string_compose ("%1/automation_name", path), ssid, auto_name, in_line, addr);
}

void
OSCRouteObserver::send_select_status (const PropertyChange& what)
{
	if (what == PropertyChange(ARDOUR::Properties::selected)) {
		if (_strip) {
			_osc.float_message_with_id ("/strip/select", ssid, _strip->is_selected(), in_line, addr);
		}
	}
}
