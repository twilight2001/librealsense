// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved.

#include "playback_sensor.h"
#include "core/motion.h"
#include <map>
#include "types.h"
#include "context.h"
#include "ds5/ds5-options.h"

class read_only_depth_scale_option : public depth_scale_option
{
    float m_value;
public:
    read_only_depth_scale_option(float value) : m_value(value){}
    bool is_read_only() const override
    {
        return true;
    }
    void set(float value) override
    {

    }
    float query() const override
    {
        return m_value;
    }
    option_range get_range() const override
    {
        return { m_value, m_value, 0, m_value};
    }
};

playback_sensor::playback_sensor(const device_interface& parent_device, const sensor_snapshot& sensor_description, uint32_t sensor_id):
    m_sensor_description(sensor_description),
    m_sensor_id(sensor_id),
    m_is_started(false),
    m_user_notification_callback(nullptr, [](rs2_notifications_callback* n) {}),
    m_parent_device(parent_device)
{
    register_sensor_streams(m_sensor_description.get_stream_profiles());
    register_sensor_infos(m_sensor_description);
    register_sensor_optiosn(m_sensor_description.get_options());
}
playback_sensor::~playback_sensor()
{
}

stream_profiles playback_sensor::get_stream_profiles()
{
    return m_available_profiles;
}

void playback_sensor::open(const stream_profiles& requests)
{
	//Playback can only play the streams that were recorded. 
	//Go over the requested profiles and see if they are available

    for (auto&& r : requests)
    {
        if (std::find_if(std::begin(m_available_profiles),
            std::end(m_available_profiles),
            [r](const std::shared_ptr<stream_profile_interface>& s) { return r->get_unique_id() == s->get_unique_id(); }) == std::end(m_available_profiles))
        {
            throw std::runtime_error("Failed to open sensor, requested profile is not available");
        }
    }
    std::vector<stream_filter> opened_streams;
	//For each stream, create a dedicated dispatching thread
    for (auto&& profile : requests)
    {
        m_dispatchers.emplace(std::make_pair(profile->get_unique_id(), std::make_shared<dispatcher>(10))); //TODO: what size the queue should be?
        m_dispatchers[profile->get_unique_id()]->start();
        stream_filter f{ m_sensor_id, profile->get_stream_type(), static_cast<uint32_t>(profile->get_stream_index()) };
        opened_streams.push_back(f);
    }

    opened(opened_streams);
}

void playback_sensor::close()
{
    std::vector<stream_filter> closed_streams;
    for (auto dispatcher : m_dispatchers)
    {
        dispatcher.second->flush();
        for (auto available_profile : m_available_profiles)
        {
            if(available_profile->get_unique_id() == dispatcher.first)
            {
                stream_filter f{ m_sensor_id, available_profile->get_stream_type(), static_cast<uint32_t>(available_profile->get_stream_index()) };
                closed_streams.push_back(f);
            }
        }      
    }
    m_dispatchers.clear();
    closed(closed_streams);
}

void playback_sensor::register_notifications_callback(notifications_callback_ptr callback)
{
    m_user_notification_callback = std::move(callback);
}

void playback_sensor::start(frame_callback_ptr callback)
{
    if (m_is_started == false)
    {
        started(m_sensor_id, callback);
        m_is_started = true;
        m_user_callback = callback;
    }
}
void playback_sensor::stop(bool invoke_required)
{
    if (m_is_started == true)
    {
        stopped(m_sensor_id, invoke_required);
        m_is_started = false;
    }
}
void playback_sensor::stop()
{
    stop(true);
}

bool playback_sensor::is_streaming() const
{
    return m_is_started;
}
bool playback_sensor::extend_to(rs2_extension extension_type, void** ext)
{
    std::shared_ptr<extension_snapshot> e = m_sensor_description.get_sensor_extensions_snapshots().find(extension_type);
    if(e == nullptr)
    {
        return false;
    }
    switch (extension_type)
    {
    case RS2_EXTENSION_UNKNOWN: return false;
    case RS2_EXTENSION_DEBUG : return try_extend<debug_interface>(e, ext);
    case RS2_EXTENSION_INFO : return try_extend<info_interface>(e, ext);
    case RS2_EXTENSION_MOTION : return try_extend<motion_sensor_interface>(e, ext);;
    case RS2_EXTENSION_OPTIONS : return try_extend<options_interface>(e, ext);;
    case RS2_EXTENSION_VIDEO : return try_extend<video_sensor_interface>(e, ext);;
    case RS2_EXTENSION_ROI : return try_extend<roi_sensor_interface>(e, ext);;
    case RS2_EXTENSION_VIDEO_FRAME : return try_extend<video_frame>(e, ext);
 //TODO: RS2_EXTENSION_MOTION_FRAME : return try_extend<motion_frame>(e, ext);
    case RS2_EXTENSION_COUNT :
        //[[fallthrough]];
    default:
        LOG_WARNING("Unsupported extension type: " << extension_type);
        assert(0);
        return false;
    }
}

const device_interface& playback_sensor::get_device()
{
    return m_parent_device;
}

void playback_sensor::handle_frame(frame_holder frame, bool is_real_time)
{
    if(frame == nullptr)
    {
        throw invalid_value_exception("null frame passed to handle_frame");
    }
    if(m_is_started)
    {
        auto type = frame->get_stream()->get_stream_type();
        auto index = static_cast<uint32_t>(frame->get_stream()->get_stream_index());
        frame->set_stream(m_streams[device_serializer::stream_identifier{ 0, 0, type, index }]);
        frame->set_sensor(shared_from_this());
        auto stream_id = frame.frame->get_stream()->get_unique_id();
		//TODO: remove this once filter is implemented (which will only read streams that were 'open'ed 
    	if(m_dispatchers.find(stream_id) == m_dispatchers.end())
		{
			return;
		}
        //TODO: Ziv, remove usage of shared_ptr when frame_holder is cpoyable
        auto pf = std::make_shared<frame_holder>(std::move(frame));
        m_dispatchers.at(stream_id)->invoke([this, pf](dispatcher::cancellable_timer t)
        {
            frame_interface* pframe = nullptr;
            std::swap((*pf).frame, pframe);
            m_user_callback->on_frame((rs2_frame*)pframe);
        });
        if(is_real_time)
        {
            m_dispatchers.at(stream_id)->flush();
        }
    }
}
void playback_sensor::flush_pending_frames()
{
    for (auto&& dispatcher : m_dispatchers)
    {
        dispatcher.second->flush();
    }
}

void playback_sensor::register_sensor_streams(const stream_profiles& profiles)
{
    for (auto profile : profiles)
    {
        profile->set_unique_id(m_parent_device.get_context()->generate_stream_id());
        //TODO:        m_parent_device.get_context()->register_extrinsics()
        m_available_profiles.push_back(profile);
        m_streams[device_serializer::stream_identifier{ 0,0, profile->get_stream_type(), static_cast<uint32_t>(profile->get_stream_index()) }] = profile;
    }
}

void playback_sensor::register_sensor_infos(const sensor_snapshot& sensor_snapshot)
{
    auto info_snapshot = sensor_snapshot.get_sensor_extensions_snapshots().find(RS2_EXTENSION_INFO);
    if (info_snapshot == nullptr)
    {
        throw io_exception("Recorded file does not contain sensor information");
    }
    auto info_api = As<info_interface>(info_snapshot);
    if (info_api == nullptr)
    {
        throw invalid_value_exception("Failed to get info interface from sensor snapshots");
    }
    for (int i = 0; i < RS2_CAMERA_INFO_COUNT; ++i)
    {
        rs2_camera_info info = static_cast<rs2_camera_info>(i);
        if (info_api->supports_info(info))
        {
            register_info(info, info_api->get_info(info));
        }
    }
}

void playback_sensor::register_sensor_optiosn(const std::map<rs2_option, float>& options)
{
    auto depth_scale_it = options.find(RS2_OPTION_DEPTH_UNITS);
    if (depth_scale_it != options.end())
    {
        register_option(depth_scale_it->first, std::make_shared<read_only_depth_scale_option>(depth_scale_it->second));
    }
}