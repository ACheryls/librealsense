// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved.

#include <rs-pointcloud-stitching.h>
#include <librealsense2/hpp/rs_internal.hpp>
#include <librealsense2/rsutil.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <int-rs-splash.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <fstream>
#include <regex>
#include <thread>
#include <windows.h>

using namespace std;
using namespace rs_pointcloud_stitching;

#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY            0x00000010  

bool dirExists(const std::string& dirName_in)
{
    DWORD ftyp = GetFileAttributesA(dirName_in.c_str());
    if (ftyp == INVALID_FILE_ATTRIBUTES)
        return false;  //something is wrong with your path!

    if (ftyp & FILE_ATTRIBUTE_DIRECTORY)
        return true;   // this is a directory!

    return false;    // this is not a directory!
}
// structure of a matrix 4 X 4, representing rotation and translation as following:
// pos_and_rot[i][j] is 
//  _                        _ 
// |           |              |
// | rotation  | translation  |
// |   (3x3)   |    (3x1)     |
// | _________ |____________  |
// |     0     |      1       |
// |_  (1x3)   |    (1x1)    _|
//
struct position_and_rotation {
    double pos_and_rot[4][4];
    // rotation tolerance - units are in cosinus of radians
    const double rotation_tolerance = 0.000001;
    // translation tolerance - units are in meters
    const double translation_tolerance = 0.000001; // 0.001mm

    position_and_rotation operator* (const position_and_rotation& other)
    {
        position_and_rotation product;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                product.pos_and_rot[i][j] = 0;
                for (int k = 0; k < 4; k++)
                    product.pos_and_rot[i][j] += pos_and_rot[i][k] * other.pos_and_rot[k][j];
            }
        }
        return product;
    }
};

position_and_rotation matrix_4_by_4_from_translation_and_rotation(const float* position, const float* rotation)
{
    position_and_rotation pos_rot;
    pos_rot.pos_and_rot[0][0] = static_cast<double>(rotation[0]);
    pos_rot.pos_and_rot[1][0] = static_cast<double>(rotation[1]);
    pos_rot.pos_and_rot[2][0] = static_cast<double>(rotation[2]);

    pos_rot.pos_and_rot[0][1] = static_cast<double>(rotation[3]);
    pos_rot.pos_and_rot[1][1] = static_cast<double>(rotation[4]);
    pos_rot.pos_and_rot[2][1] = static_cast<double>(rotation[5]);

    pos_rot.pos_and_rot[0][2] = static_cast<double>(rotation[6]);
    pos_rot.pos_and_rot[1][2] = static_cast<double>(rotation[7]);
    pos_rot.pos_and_rot[2][2] = static_cast<double>(rotation[8]);

    pos_rot.pos_and_rot[3][0] = 0.0;
    pos_rot.pos_and_rot[3][1] = 0.0;
    pos_rot.pos_and_rot[3][2] = 0.0;

    pos_rot.pos_and_rot[0][3] = static_cast<double>(position[0]);
    pos_rot.pos_and_rot[1][3] = static_cast<double>(position[1]);
    pos_rot.pos_and_rot[2][3] = static_cast<double>(position[2]);

    pos_rot.pos_and_rot[3][3] = 1.0;

    return pos_rot;
}

rs2_extrinsics matrix_to_extrinsics(const position_and_rotation& pos_rot)
{
    rs2_extrinsics res = {
        {
        (float)pos_rot.pos_and_rot[0][0],
        (float)pos_rot.pos_and_rot[1][0],
        (float)pos_rot.pos_and_rot[2][0],

        (float)pos_rot.pos_and_rot[0][1],
        (float)pos_rot.pos_and_rot[1][1],
        (float)pos_rot.pos_and_rot[2][1],

        (float)pos_rot.pos_and_rot[0][2],
        (float)pos_rot.pos_and_rot[1][2],
        (float)pos_rot.pos_and_rot[2][2]
        },
        {
        (float)pos_rot.pos_and_rot[0][3],
        (float)pos_rot.pos_and_rot[1][3],
        (float)pos_rot.pos_and_rot[2][3]
        }
    };
    return res;
}

bool parse_configuration(const std::string& line, const std::vector<std::string>& tokens,
    rs2_stream& type, int& width, int& height, rs2_format& format, int& fps, int& index)
{
    bool res = false;
    try
    {
        auto tokens = tokenize(line, ',');
        if (tokens.size() < e_stream_index)
            return res;

        // Convert string to uppercase
        type = parse_stream_type(to_lower(tokens[e_stream_type]));
        width = parse_number(tokens[e_res_width].c_str());
        height = parse_number(tokens[e_res_height].c_str());
        fps = parse_fps(tokens[e_fps]);
        format = parse_format(to_lower(tokens[e_format]));
        // Backward compatibility
        if (tokens.size() > e_stream_index)
            index = parse_number(tokens[e_stream_index].c_str());
        res = true;
        std::cout << "Request added for " << line << std::endl;
    }
    catch (...)
    {
        std::cout << "Invalid syntax in configuration line " << line << std::endl;
    }

    return res;
}

std::vector<stream_request> parse_profiles_file(const std::string& config_filename, bool& is_left)
{
    is_left = false;
    std::vector<stream_request> user_requests;

    ifstream file(config_filename);

    if (!file.is_open())
        throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " was not found!");

    // Line starting with non-alpha characters will be treated as comments
    const static std::regex starts_with("^[a-zA-Z]");
    string line;
    rs2_stream stream_type;
    rs2_format format;
    int width{}, height{}, fps{}, index{};

    // Parse the config file
    while (getline(file, line))
    {
        auto tokens = tokenize(line, ',');

        if (std::regex_search(line, starts_with))
        {
            if (tokens[0] == "left")
            {
                is_left = true;
                continue;
            }
            if (parse_configuration(line, tokens, stream_type, width, height, format, fps, index))
                user_requests.push_back({ stream_type, format, width, height, fps,  index });
        }
    }

    // Sanity test agains multiple conflicting requests for the same sensor
    if (user_requests.size())
    {
        std::sort(user_requests.begin(), user_requests.end(),
            [](const stream_request& l, const stream_request& r) { return l._stream_type < r._stream_type; });

        for (auto i = 0; i < user_requests.size() - 1; i++)
        {
            if ((user_requests[i]._stream_type == user_requests[i + 1]._stream_type) && ((user_requests[i]._stream_idx == user_requests[i + 1]._stream_idx)))
                throw runtime_error(stringify() << "Invalid configuration file - multiple requests for the same sensor:\n"
                    << user_requests[i] << user_requests[+i]);
        }
    }
    else
        throw std::runtime_error(stringify() << "Invalid configuration file - " << config_filename << " zero requests accepted");

    return user_requests;
}

void CPointcloudStitcher::parse_calibration_file(const std::string& config_filename)
{
    ifstream file(config_filename);

    if (!file.is_open())
        throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " was not found!");

    // Line starting with non-alpha characters will be treated as comments

    const static std::regex starts_with("^[a-zA-Z0-9]");
    string line;

    std::vector<std::string> serials;
    for (auto serial : _wanted_profiles)
        serials.push_back(serial.first);
    
    // Parse the config file
    while (getline(file, line))
    {
        if (std::regex_search(line, starts_with))
        {
            auto tokens = tokenize(line, ',');
            if (tokens.size() != 14)
            {
                std::cout << "invalid configuration line: " << line << std::endl;
                continue;
            }
            std::string from_serial(tokens[0]);
            if (from_serial == "virtual_dev") from_serial = _serial_vir;
            else if (std::count(serials.begin(), serials.end(), from_serial) == 0) continue;

            std::string to_serial(tokens[1]);
            if (to_serial == "virtual_dev") to_serial = _serial_vir;
            else if (std::count(serials.begin(), serials.end(), to_serial) == 0) continue;

            rs2_extrinsics crnt_extrinsics({ { 0,0,0,0,0,0,0,0,0 }, { 0,0,0 } });
            int count(0);
            for (auto it = tokens.begin() + 2; it != tokens.end(); ++it, count++) {
                std::string token = *it;
                if (count <= 9)
                    crnt_extrinsics.rotation[count] = stof(token);
                else
                    crnt_extrinsics.translation[count-9] = stof(token);
                if (count == 12)
                    break;
            }
            if (count == 12)
                _ir_extrinsics[from_serial][to_serial] = crnt_extrinsics;
        }
    }
    if (_ir_extrinsics.empty())
        throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " contains less then 1 full transformation!");
    if (_ir_extrinsics.size() < 2)
        throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " does not contain 2 full transformation!");
    // Fill calibration from exery camera to the virtual one:

    // Check that from every device there is, even passing through another, a transformation to virtual_dev
    for (auto serial : serials)
    {
        try
        {
            std::map<std::string, rs2_extrinsics >& this_transformations = _ir_extrinsics.at(serial);
            if (this_transformations.find(_serial_vir) == this_transformations.end())
            {
                for (auto trans_to_other : this_transformations)
                {
                    try
                    {
                        rs2_extrinsics this_to_other = trans_to_other.second;
                        rs2_extrinsics other_to_virtual = _ir_extrinsics.at(trans_to_other.first).at(_serial_vir);
                        position_and_rotation pr_t_to_o = matrix_4_by_4_from_translation_and_rotation(this_to_other.translation, this_to_other.rotation);
                        position_and_rotation pr_o_to_v = matrix_4_by_4_from_translation_and_rotation(other_to_virtual.translation, other_to_virtual.rotation);
                        position_and_rotation pr_t_to_v = pr_t_to_o * pr_o_to_v;
                        this_transformations.insert(std::pair<std::string, rs2_extrinsics>(_serial_vir, matrix_to_extrinsics(pr_t_to_v)));
                        break;
                    }
                    catch (const std::out_of_range& e)
                    {
                        std::cout << "Given .cfg configure file " << config_filename << " does not contain transformation: " << e.what() << std::endl;
                    }
                }
                if (this_transformations.find(_serial_vir) == this_transformations.end())
                {
                    throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " does not contain a connection from " << serial << " to virtual_dev.");
                }
            }
        }
        catch (const std::out_of_range&)
        {
            throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " does not contain transformation from " << serial << ".");
        }
    }
}

void CPointcloudStitcher::parse_virtual_device_config_file(const std::string& config_filename)
{
    ifstream file(config_filename);

    if (!file.is_open())
        throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " was not found!");

    // Line starting with non-alpha characters will be treated as comments

    const static std::regex starts_with("^[a-zA-Z]");
    string line;

    // Parse the config file
    while (getline(file, line))
    {
        if (std::regex_search(line, starts_with))
        {
            auto tokens = tokenize(line, '=');
            if (tokens.size() != 2)
                continue;
            _virtual_dev_params.insert(std::pair<std::string, double > (tokens[0], stod(tokens[1])));
        }
    }
    // Test that all values where assigned:
    std::vector<std::string> needed_keys{ "depth_width", "depth_height", "depth_fov_x", "depth_fov_y", "color_width", "color_height", "color_fov_x", "color_fov_y" };
    for (auto key : needed_keys)
    {
        if (_virtual_dev_params.find(key) == _virtual_dev_params.end())
        {
            throw runtime_error(stringify() << "Given .cfg configure file " << config_filename << " does not contain value for: " << key);
        }
    }
}


CPointcloudStitcher::CPointcloudStitcher(const std::string& working_dir, const std::string& calibration_file) :
    _working_dir(working_dir),
    _calibration_file(calibration_file),
    _serial_vir("12345678"),
    _frame_number(0),
    _is_recording(false)
{
    // init frames map
    //for initilize only - an empty frame with its properties
    rs2::frame frame;

    //set each frame with its properties:
    //  { tile's x coordinate, tiles's y coordinate, tile's width (in tiles), tile's height (in tiles), priority (default value=0) }, (x=0,y=0) <-> left bottom corner
    //priority sets the order of drawing frame when two frames share part of the same tile, 
    //meaning if there are two frames: frame1 with priority=-1 and frame2 with priority=0, both with { 0,0,1,1 } as property,
    //frame2 will be drawn on top of frame1
    _frames_map[COLOR1] = frame_and_tile_property(frame, { 0,0,1,1,Priority::high });
    _frames_map[COLOR_UNITED] = frame_and_tile_property(frame, { 1,0,2,2,Priority::high });
    _frames_map[COLOR2] = frame_and_tile_property(frame, { 3,0,1,1,Priority::high });
    _frames_map[DEPTH1] = frame_and_tile_property(frame, { 0,2,1,1,Priority::high });
    _frames_map[DEPTH_UNITED] = frame_and_tile_property(frame, { 1,2,2,2,Priority::high });
    _frames_map[DEPTH2] = frame_and_tile_property(frame, { 3,2,1,1,Priority::high });
}

bool CPointcloudStitcher::OpenSensors(std::shared_ptr<rs2::device> dev)
{
    size_t profiles_found(0);
    std::string serial(dev->get_info(RS2_CAMERA_INFO_SERIAL_NUMBER));
    std::vector<stream_request> dev_wanted_profiles = _wanted_profiles[serial];
    std::vector<stream_request> requests_to_go = dev_wanted_profiles;
    std::vector<rs2::stream_profile> matches;

    // Configure and starts streaming
    for (auto&& sensor : dev->query_sensors())
    {
        for (auto& profile : sensor.get_stream_profiles())
        {
            // All requests have been resolved
            if (!requests_to_go.size())
                break;

            // Find profile matches
            auto fulfilled_request = std::find_if(requests_to_go.begin(), requests_to_go.end(), [&matches, profile](const stream_request& req)
                {
                    bool res = false;
                    if ((profile.stream_type() == req._stream_type) &&
                        (profile.format() == req._stream_format) &&
                        (profile.stream_index() == req._stream_idx) &&
                        (profile.fps() == req._fps))
                    {
                        if (auto vp = profile.as<rs2::video_stream_profile>())
                        {
                            if ((vp.width() != req._width) || (vp.height() != req._height))
                                return false;
                        }
                        res = true;
                        matches.push_back(profile);
                    }

                    return res;
                });

            // Remove the request once resolved
            if (fulfilled_request != requests_to_go.end())
                requests_to_go.erase(fulfilled_request);
        }

        // Aggregate resolved requests
        if (matches.size())
        {
            profiles_found += matches.size();
            sensor.open(matches);
            _active_sensors[serial].emplace_back(sensor);
            matches.clear();
        }
    }
    if (profiles_found == dev_wanted_profiles.size())
    {
        return true;
    }
    else
    {
        std::cerr << "ERROR: Managed to validate only %d/%d profiles for dev: " << serial << std::endl;
        return false;
    }
}

bool CPointcloudStitcher::Init()
{
    {
        std::stringstream filename;
        filename << _working_dir << "/" << "virtual_dev.cfg";
        parse_virtual_device_config_file(filename.str());
    }
    rs2::context ctx;
    rs2::device_list list;

    list = ctx.query_devices();

    if (list.size() != 2)
    {
        std::cout << "Connect 2 Realsense Cameras to proceed" << std::endl;
        return false;
    }

    for (auto dev_info : list)
    {
        _devices.push_back(std::make_shared<rs2::device>(dev_info));
    }

    std::vector<std::string> serials;
    for (auto dev : _devices)
    {
        bool is_left;
        std::string dev_name = dev->get_info(RS2_CAMERA_INFO_NAME);
        std::string dev_serial = dev->get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
        serials.push_back(dev_serial);
        std::cout << "Got Device: " << dev_name << " : " << dev_serial << std::endl;
        std::stringstream filename;
        filename << _working_dir << "/" << dev_serial << ".cfg";
        _wanted_profiles[dev_serial] = parse_profiles_file(filename.str(), is_left);
        if (is_left)
        {
            std::cout << "Device : " << dev_serial << " will be presented on the left side" << std::endl;
            _left_device = dev_serial;  // TODO: define left device based on transformation between devices.
        }
    }
    {
        std::stringstream filename;
        filename << _working_dir << "/" << _calibration_file;
        parse_calibration_file(filename.str());
    }
    // This is a rotation matrix of 30 degrees around z-axis:
    //array([[ 0.8660254, -0.5, 0. ],
    //    [0.5, 0.8660254, 0.],
    //    [0., 0., 1.]] )

    // This is a rotation matrix of 15 degrees around z-axis: (== virtual to left camera)
    //array([[ 0.96592583, -0.25881905, 0. ],
        //[0.25881905, 0.96592583, 0.],
        //[0., 0., 1.]] )
    CreateVirtualDevice();

    //_ir_extrinsics[serials[0]][_serial_vir] = rs2_extrinsics({ {0.96592583, 0,  0.25881905,  0,1,0,      -0.25881905, 0., 0.96592583}, {0,0,0} });   //15 Around Y axis ?
    //_ir_extrinsics[serials[1]][_serial_vir] = rs2_extrinsics({ {0.96592583, 0, -0.25881905, 0,1,0,        0.25881905,  0., 0.96592583}, {0,0,0} });

    //_ir_extrinsics[serials[0]][_serial_vir] = rs2_extrinsics({ {0.8660254, 0,  0.5,  0,1,0,      -0.5, 0., 0.8660254}, {0,0,0} });   //30 Around Y axis ?
    //_ir_extrinsics[serials[1]][_serial_vir] = rs2_extrinsics({ {0.8660254, 0, -0.5, 0,1,0,        0.5,  0., 0.8660254}, {0,0,0} });

    //_ir_extrinsics[serials[0]][_serial_vir] = rs2_extrinsics({ {1,0,0,   0,1,0,  0,0,1}, {0,0,0} });
    //_ir_extrinsics[serials[1]][_serial_vir] = rs2_extrinsics({ {1,0,0,   0,1,0,  0,0,1}, {0,0,0} });

    return true;
}

rs2_intrinsics CPointcloudStitcher::create_intrinsics(const synthetic_frame& _virtual_frame, const float fov_x, const float fov_y)    // fov_x, fov_y in radians.
{
    float fx = _virtual_frame.x / (2 * tanf(fov_x / 2));
    float fy = _virtual_frame.y / (2 * tanf(fov_y / 2));
    rs2_intrinsics intrinsics = { _virtual_frame.x, _virtual_frame.y,
        (float)_virtual_frame.x / 2, (float)_virtual_frame.y / 2,
        fx , fy ,
        RS2_DISTORTION_BROWN_CONRADY ,{ 0,0,0,0,0 } };

    return intrinsics;
}


void CPointcloudStitcher::InitializeVirtualFrames()
{
    _virtual_depth_frame.x = static_cast<int>(_virtual_dev_params.at("depth_width"));
    _virtual_depth_frame.y = static_cast<int>(_virtual_dev_params.at("depth_height"));
    _virtual_depth_frame.bpp = 2;

    _virtual_color_frame.x = static_cast<int>(_virtual_dev_params.at("color_width"));
    _virtual_color_frame.y = static_cast<int>(_virtual_dev_params.at("color_height"));
    _virtual_color_frame.bpp = 4;


    std::vector<uint8_t> pixels_depth((size_t)_virtual_depth_frame.x * _virtual_depth_frame.y * _virtual_depth_frame.bpp, 0);
    _virtual_depth_frame.frame = std::move(pixels_depth);

    std::vector<uint8_t> pixels_color((size_t)_virtual_color_frame.x * _virtual_color_frame.y * _virtual_color_frame.bpp, 0);

    _virtual_color_frame.frame = std::move(pixels_color);
}

void CPointcloudStitcher::CreateVirtualDevice()
{
    InitializeVirtualFrames();

    float fov_x(float(_virtual_dev_params.at("color_fov_x") * PI / 180.0));
    float fov_y(float(_virtual_dev_params.at("color_fov_y") * PI / 180.0));
    rs2_intrinsics color_intrinsics = create_intrinsics(_virtual_color_frame, fov_x, fov_y);
    fov_x = float(_virtual_dev_params.at("depth_fov_x") * PI / 180.0);
    fov_y = float(_virtual_dev_params.at("depth_fov_y") * PI / 180.0);
    rs2_intrinsics depth_intrinsics = create_intrinsics(_virtual_depth_frame, fov_x, fov_y);

    //==================================================//
    //           Declare Software-Only Device           //
    //==================================================//

    _soft_dev.register_info(RS2_CAMERA_INFO_SERIAL_NUMBER, _serial_vir);
    auto depth_sensor = _soft_dev.add_sensor("Depth"); // Define single sensor
    _active_software_sensors.insert(std::pair<std::string, rs2::software_sensor>("Depth", depth_sensor));
    auto color_sensor = _soft_dev.add_sensor("Color"); // Define single sensor
    _active_software_sensors.insert(std::pair<std::string, rs2::software_sensor>("Color", color_sensor));

    auto depth_stream = depth_sensor.add_video_stream({ RS2_STREAM_DEPTH, 0, 0,
                                _virtual_depth_frame.x, _virtual_depth_frame.y, 30, _virtual_depth_frame.bpp,
                                RS2_FORMAT_Z16, depth_intrinsics });

    depth_sensor.add_read_only_option(RS2_OPTION_DEPTH_UNITS, 0.001f);


    auto color_stream = color_sensor.add_video_stream({ RS2_STREAM_COLOR, 0, 1, 
                                _virtual_color_frame.x, _virtual_color_frame.y, 30, _virtual_color_frame.bpp,
                                RS2_FORMAT_RGBA8, color_intrinsics });

    depth_stream.register_extrinsics_to(color_stream, { { 1,0,0,0,1,0,0,0,1 },{ 0,0,0 } });
    _soft_dev.create_matcher(RS2_MATCHER_DLR_C);
}

bool CPointcloudStitcher::Start()
{
    int count(0);
    for (auto dev : _devices)
    {
        if (!OpenSensors(dev))
        {
            CloseSensors();
            return false;
        }
    }

    for (auto sensors : _active_sensors)
    {
        const string& serial = sensors.first;
        _frames[serial] = rs2::frame_queue(2, true);

        _syncer[sensors.first].start([this, serial](rs2::frame frame) {
            frame_callback(frame, serial); });

        for (auto sensor : sensors.second)
        {
            std::cout << "Starting sensor: " << sensor.get_info(RS2_CAMERA_INFO_NAME) << std::endl;
            sensor.start(_syncer[sensors.first]);
            std::cout << "Ok:" << std::endl;
        }
    }

    std::string serial(_soft_dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER));
    for (auto&& sensor : _soft_dev.query_sensors())
    {
        sensor.open(sensor.get_stream_profiles());
        _active_sensors[serial].emplace_back(sensor);
        std::cout << "ADD SENSOR TO SYNC" << std::endl;
        sensor.start(_soft_sync);
    }
    return true;
}

void CPointcloudStitcher::StopSensors()
{
    for (auto sensors : _active_sensors)
    {
        for (auto sensor : sensors.second)
        {
            sensor.stop();
        }
    }
}

void CPointcloudStitcher::CloseSensors()
{
    for (auto sensors : _active_sensors)
    {
        for (auto sensor : sensors.second)
        {
            sensor.close();
        }
    }
}

void CPointcloudStitcher::frame_callback(rs2::frame frame, const string& serial)
{
    rs2::frameset fset = frame.as<rs2::frameset>();
    if (fset.size() == _wanted_profiles[serial].size())
    {
        _frames[serial].enqueue(std::move(fset));
    }
}

void multiply_vector4_by_matrix_4x3(const GLfloat vec[], const GLfloat mat[], GLfloat* result)
{
    const auto M = 4;
    const auto N = 3;
    int max_idx(0);
    for (int i = 0; i < N; i++)
    {
        result[i] = 0;
        for (int j = 0; j < M; j++)
        {
            result[i] += vec[j] * mat[N * j + i];
            max_idx = std::max(max_idx, N * j + i);
        }
    }
    std::cout << "max_idx: " << max_idx << std::endl;
    return;
}

void CPointcloudStitcher::ProjectFramesOnOtherDevice(rs2::frameset frames, const string& from_serial, const string& to_serial)
{
    rs2::points points;
    rs2::frame depth = frames.first_or_default(RS2_STREAM_DEPTH);
    if (!depth) return;
    rs2::frame color = frames.first_or_default(RS2_STREAM_COLOR);
    float depth_point[3] = { 0 };
    float virtual_point[3] = { 0 };
    float color_pixel[2] = { 0 };
    float virtual_depth_pixel[2] = { 0 };
    float virtual_color_pixel[2] = { 0 };

    rs2::software_sensor virtual_depth_sensor = _active_software_sensors.at("Depth");
    const rs2_extrinsics& extrinsics(_ir_extrinsics[from_serial][to_serial]);
    const rs2_intrinsics& virtual_depth_intinsics(virtual_depth_sensor.get_active_streams()[0].as<rs2::video_stream_profile>().get_intrinsics());
    rs2::software_sensor virtual_color_sensor = _active_software_sensors.at("Color");
    const rs2_intrinsics& virtual_color_intinsics(virtual_color_sensor.get_active_streams()[0].as<rs2::video_stream_profile>().get_intrinsics());

    rs2_intrinsics color_intinsics;
    int color_bpp(0);
    if (color)
    {
        rs2::video_frame color_frame = color.as<rs2::video_frame>();
        color_intinsics = (color_frame.get_profile().as<rs2::video_stream_profile>().get_intrinsics());
        color_bpp = color_frame.get_bytes_per_pixel();
    }

    if (auto as_depth = depth.as<rs2::depth_frame>())
    {
        const rs2_intrinsics& depth_intinsics(as_depth.get_profile().as<rs2::video_stream_profile>().get_intrinsics());
        //{
        //    float HFOV = (2 * atan(depth_intinsics.width / (2 * depth_intinsics.fx))) * 180.0 / PI;
        //    float VFOV = (2 * atan(depth_intinsics.height / (2 * depth_intinsics.fy))) * 180.0 / PI;
        //    std::cout << "Depth Fov: " << HFOV << "x" << VFOV << std::endl;
        //}
        //{
        //    float HFOV = (2 * atan(color_intinsics.width / (2 * color_intinsics.fx))) * 180.0 / PI;
        //    float VFOV = (2 * atan(color_intinsics.height / (2 * color_intinsics.fy))) * 180.0 / PI;
        //    std::cout << "Color Fov: " << HFOV << "x" << VFOV << std::endl;
        //}

        uint8_t* zero_buf[3] = { 0,0,0 };

        const uint16_t* ptr = (const uint16_t * )as_depth.get_data();
        for (int y = 0; y < as_depth.get_height(); y++)
        {
            for (int x = 0; x < as_depth.get_width(); x++, ptr++)
            {
                float pixel[] = { (float)x, (float)y };
                if (as_depth.get_distance(x, y) <= 0)
                    continue;

                rs2_deproject_pixel_to_point(depth_point, &depth_intinsics, pixel, as_depth.get_distance(x, y));
                rs2_transform_point_to_point(virtual_point, &extrinsics, depth_point);
                rs2_project_point_to_pixel(virtual_depth_pixel, &virtual_depth_intinsics, virtual_point);

                if (virtual_depth_pixel[0] < 1 || virtual_depth_pixel[0] >= virtual_depth_intinsics.width ||
                    virtual_depth_pixel[1] < 1 || virtual_depth_pixel[1] >= virtual_depth_intinsics.height )
                {
                    continue;
                }
                int offset((int)(virtual_depth_pixel[1]) * _virtual_depth_frame.x + (int)(virtual_depth_pixel[0]));
                uint16_t crnt_depth_val(((uint16_t*)_virtual_depth_frame.frame.data())[offset]);
                uint8_t* virtual_color_ptr(NULL);

                bool is_filling_depth(!crnt_depth_val || crnt_depth_val > *ptr);
                if (is_filling_depth)
                {
                    ((uint16_t*)_virtual_depth_frame.frame.data())[offset] = *ptr;
                    // Padding is backwards so not creating the appearance of an existing depth value to consider when testing next pixel.
                    ((uint16_t*)_virtual_depth_frame.frame.data())[offset - 1] = *ptr;
                    ((uint16_t*)_virtual_depth_frame.frame.data())[offset - _virtual_depth_frame.x] = *ptr;
                    ((uint16_t*)_virtual_depth_frame.frame.data())[offset - (_virtual_depth_frame.x + 1)] = *ptr;
                }

                bool missing_color(false);
                if (color)
                {
                    rs2_project_point_to_pixel(color_pixel, &color_intinsics, depth_point);
                    if (color_pixel[0] >= 0 && color_pixel[0] < color_intinsics.width &&
                        color_pixel[1] >= 0 && color_pixel[1] < color_intinsics.height)
                    {
                        rs2_project_point_to_pixel(virtual_color_pixel, &virtual_color_intinsics, virtual_point);
                        if (virtual_color_pixel[0] >= 0 && virtual_color_pixel[0] < virtual_color_intinsics.width-1 &&
                            virtual_color_pixel[1] >= 0 && virtual_color_pixel[1] < virtual_color_intinsics.height-1)
                        {
                            int offset_dest(((int)virtual_color_pixel[1] * _virtual_color_frame.x + (int)virtual_color_pixel[0]) * _virtual_color_frame.bpp);
                            virtual_color_ptr = (uint8_t*)_virtual_color_frame.frame.data() + offset_dest;
                            missing_color = (0 == memcmp(virtual_color_ptr, zero_buf, 3));
                        }
                    }
                }

                if (virtual_color_ptr && (is_filling_depth || missing_color))
                {
                    int offset_src(((int)color_pixel[1] * color_intinsics.width + (int)color_pixel[0]) * color_bpp);
                    memcpy(virtual_color_ptr, (uint8_t*)(color.get_data()) + offset_src, color_bpp * sizeof(uint8_t));

                    memcpy(virtual_color_ptr + 1 * (_virtual_color_frame.bpp * sizeof(uint8_t)), (uint8_t*)(color.get_data()) + offset_src, color_bpp * sizeof(uint8_t));
                    memcpy(virtual_color_ptr + _virtual_color_frame.x * (_virtual_color_frame.bpp * sizeof(uint8_t)), (uint8_t*)(color.get_data()) + offset_src, color_bpp * sizeof(uint8_t));
                    memcpy(virtual_color_ptr + (1 + _virtual_color_frame.x) * (_virtual_color_frame.bpp * sizeof(uint8_t)), (uint8_t*)(color.get_data()) + offset_src, color_bpp * sizeof(uint8_t));
                }
            }
        }
    }
}

void CPointcloudStitcher::SaveOriginImages(const std::map<std::string, rs2::frame>& frames_sets)
{
    unsigned long long first_frame_number(0);
    for (auto frames : frames_sets)
    {
        rs2::frame frame = frames.second.as<rs2::frameset>().first_or_default(RS2_STREAM_INFRARED);
        auto vf = frame.as<rs2::video_frame>();
        std::stringstream png_file;
        png_file << _working_dir << "/" << frames.first;
        if (!dirExists(png_file.str()))
        {
            std::cout << "Create directory: " << png_file.str() << std::endl;
            CreateDirectoryA(png_file.str().c_str(), NULL);
        }
        if (!first_frame_number)
        {
            first_frame_number = frame.get_frame_number();
        }
        png_file << "/" << "img_" << first_frame_number << ".png";
        stbi_write_png(png_file.str().c_str(), vf.get_width(), vf.get_height(),
            vf.get_bytes_per_pixel(), vf.get_data(), vf.get_stride_in_bytes());
        {
            // Save intrinsics to file:
            std::stringstream filename;
            filename << _working_dir << "/" << "intrinsics_" << frames.first << ".txt";

            std::ofstream csv(filename.str());

            auto intrinsics = vf.get_profile().as<rs2::video_stream_profile>().get_intrinsics();
            csv << "sizeX, sizeY, fx, fy, ppx, ppy, distortion" << std::fixed << std::setprecision(6) << std::endl;
            csv << intrinsics.width << ", ";
            csv << intrinsics.height << ", ";
            csv << intrinsics.fx << ", ";
            csv << intrinsics.fy << ", ";
            csv << intrinsics.ppx << ", ";
            csv << intrinsics.ppy << ", ";
            for (int i = 0; i < 5; i++)
                csv << intrinsics.coeffs[0] << ", ";
            csv << std::endl;

        }
        std::cout << "Saved " << png_file.str() << std::endl;
    }
}

void CPointcloudStitcher::StartRecording(const std::string& path)
{
    if (_recorder != nullptr)
    {
        return; //already recording
    }

    try
    {
        _recorder = std::make_shared<rs2::recorder>(path, _soft_dev, false);
        _is_recording = true;
    }
    catch (const std::exception& e)
    {
        std::cout << "Error recording: " << e.what() << std::endl;
    }
}

void CPointcloudStitcher::StopRecording()
{
    auto saved_to_filename = _recorder->filename();
    _recorder.reset();
    _is_recording = false;
    std::cout << "Saved recording to:\n" << saved_to_filename << std::endl;
}

void CPointcloudStitcher::DrawTitles(const double fps, const ImVec2& window_size)
{
    ImGui::SetNextWindowSize({ 200, 25 });
    float tile_width(window_size.x / 4);
    float text_width(200);
    ImGui::SetNextWindowPos({ 0.15f * window_size.x + 0 * tile_width + (tile_width - text_width) / 2, 40 });
    ImGui::Begin("left_title", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Text((std::string("Left Camera:") + _left_device).c_str());
    ImGui::End();

    ImGui::SetNextWindowSize({ 200, 25 });
    ImGui::SetNextWindowPos({ 0.15f * window_size.x + 2 * tile_width + (tile_width - text_width) / 2, 40 });
    ImGui::Begin("right_title", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Text("Right Camera");
    ImGui::End();

    ImGui::SetNextWindowSize({ 200, 25 });
    ImGui::SetNextWindowPos({ 0.15f * window_size.x + 1 * tile_width + (tile_width - text_width) / 2, 40 });
    ImGui::Begin("virtual_camera_title", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Text("Combined Camera");
    ImGui::End();

    ImGui::SetNextWindowSize({ 200, 25 });
    ImGui::SetNextWindowPos({ 20, 40 });
    ImGui::Begin("fps", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::Text("FPS: %.2f", fps);
    ImGui::End();
}

void CPointcloudStitcher::RecordButton(const ImVec2& window_size)
{
    static const ImVec4 light_blue(0.0f, 174.0f / 255, 239.0f / 255, 255.0f / 255); // Light blue color for selected elements such as play button glyph when paused
    static const ImVec4 light_grey((0xc3) / 255.0f, (0xd5) / 255.0f, (0xe5) / 255.0f, (0xff) / 255.0f); // Text

    ////////////////////////////////////////
    // Draw recording icon
    ////////////////////////////////////////
    std::string recorod_button_name(_is_recording ? "Stop Recording" : "Record");
    auto record_button_color = _is_recording ? light_blue : light_grey;
    ImGui::PushStyleColor(ImGuiCol_Text, record_button_color);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, record_button_color);

    ImGui::SetNextWindowPos({ (window_size.x - 280) / 2,  window_size.y - 40 });
    ImGui::Begin("record_button", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (ImGui::ButtonEx(recorod_button_name.c_str(), { 280, 25 }))
    {
        if (_is_recording) //is_recording is changed inside stop/start_recording
        {
            StopRecording();
        }
        else
        {
            StartRecording("record.bag");
        }
        std::cout << "_is_recording: " << _is_recording << std::endl;
    }
    if (ImGui::IsItemHovered())
    {
        std::string record_button_hover_text = (_is_recording ? "Stop Recording" : "Start Recording");
        ImGui::SetTooltip("%s", record_button_hover_text.c_str());
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
}

void CPointcloudStitcher::SaveFramesButton(const std::map<std::string, rs2::frame>& frames_sets, const ImVec2& window_size)
{
    static const ImVec4 light_blue(0.0f, 174.0f / 255, 239.0f / 255, 255.0f / 255); // Light blue color for selected elements such as play button glyph when paused
    static const ImVec4 light_grey((0xc3) / 255.0f, (0xd5) / 255.0f, (0xe5) / 255.0f, (0xff) / 255.0f); // Text

    ////////////////////////////////////////
    // Draw recording icon
    ////////////////////////////////////////
    std::string button_name("Save Frames");
    auto button_color = light_blue;
    ImGui::PushStyleColor(ImGuiCol_Text, button_color);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, button_color);

    ImGui::SetNextWindowPos({ 100,  window_size.y - 40 });
    ImGui::Begin("save_origin_button", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (ImGui::ButtonEx(button_name.c_str(), { 280, 25 }))
    {
        SaveOriginImages(frames_sets);
    }
    if (ImGui::IsItemHovered())
    {
        std::string button_hover_text = "Save original images";
        ImGui::SetTooltip("%s", button_hover_text.c_str());
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
}

void CPointcloudStitcher::Run(window& app)
{
    const auto SLEEP_TIME = std::chrono::milliseconds(30);
    bool display_original_images(true), display_output_images(true);

    int frame_number(0);
    int count(0);
    double fps(0);
    auto start_time = std::chrono::system_clock::now();
    while (app)
    {
        auto end_time = std::chrono::system_clock::now();
        count++;
        std::chrono::duration<double> diff = end_time - start_time;
        if (diff.count() >= 1.0)
        {
            fps = count / diff.count();
            std::cout << "FPS: " << fps << std::endl;
            count = 0;
            start_time = end_time;
        }
        int offset(0);
        memset(_virtual_depth_frame.frame.data(), 0, (size_t)_virtual_depth_frame.x * _virtual_depth_frame.y * _virtual_depth_frame.bpp);
        memset(_virtual_color_frame.frame.data(), 0, (size_t)_virtual_color_frame.x * _virtual_color_frame.y * _virtual_color_frame.bpp);
        std::map<std::string, rs2::frame>     frames_sets;
        for (auto frames : _frames)
        {
            if (frames_sets.empty())
            {
                frames_sets.insert(std::pair<std::string, rs2::frame>(frames.first, frames.second.wait_for_frame()));    // Wait for next set of frames from the camera
            }
            else
            {
                rs2::frame frm;
                bool res = frames.second.try_wait_for_frame(&frm, 15);
                if (res) {
                    frames_sets.insert(std::pair<std::string, rs2::frame>(frames.first, frm));    // Wait for next set of frames from the camera
                }
                else
                {
                    std::cout << "failed 2nd frame" << std::endl;
                }
            }
        }
        if (frames_sets.size() < 2) continue;
        for (auto frames : frames_sets)
        {
            rs2::frame frm = frames.second;    // Wait for next set of frames from the camera
            auto start_project_time = std::chrono::system_clock::now();
            ProjectFramesOnOtherDevice(frm, frames.first, _serial_vir);
            auto end_project_time = std::chrono::system_clock::now();
            //std::cout << "Processing took: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_project_time - start_project_time).count() << std::endl;

            // DISPLAY ORIGINAL IMAGES
            if (display_original_images)
            {
                for (auto frame : frm.as<rs2::frameset>())
                {
                    if (_left_device == frames.first) offset = 0; else offset = 2;
                    if (frame.is<rs2::depth_frame>())
                    {
                        offset += 3;

                        _frames_map[offset].first = frame.apply_filter(_colorizer);
                    }
                    else
                    {
                        _frames_map[offset].first = frame;
                    }
                }
            }
        }

        {
            rs2::frame aframe = frames_sets.begin()->second;
            rs2::software_sensor virtual_depth_sensor = _active_software_sensors.at("Depth");
            virtual_depth_sensor.on_video_frame({ _virtual_depth_frame.frame.data(), // Frame pixels from capture API
                [](void*) {}, // Custom deleter (if required)
                _virtual_depth_frame.x * _virtual_depth_frame.bpp, _virtual_depth_frame.bpp, // Stride and Bytes-per-pixel
                aframe.get_timestamp(), aframe.get_frame_timestamp_domain(), _frame_number, // Timestamp, Frame# for potential sync services
                *(virtual_depth_sensor.get_active_streams().begin()) });
        }
        rs2::frame aframe = frames_sets.begin()->second;
        rs2::frame color = aframe.as<rs2::frameset>().first_or_default(RS2_STREAM_COLOR);
        if (color)
        {
            rs2::software_sensor virtual_color_sensor = _active_software_sensors.at("Color");
            virtual_color_sensor.on_video_frame({ _virtual_color_frame.frame.data(), // Frame pixels from capture API
                [](void*) {}, // Custom deleter (if required)
                _virtual_color_frame.x * _virtual_color_frame.bpp, _virtual_color_frame.bpp, // Stride and Bytes-per-pixel
                color.get_timestamp(), color.get_frame_timestamp_domain(), _frame_number, // Timestamp, Frame# for potential sync services
                *(virtual_color_sensor.get_active_streams().begin()) });
        }
        rs2::frameset fset;
        if (!_soft_sync.try_wait_for_frames(&fset))
        {
            std::cout << "Failed to get processed frame." << std::endl;
            continue;
        }

#if false
        std::cout << "fset.size(): " << fset.size() << std::endl;
        for (auto it = fset.begin(); it != fset.end(); ++it)
        {
            auto f = (*it);
            auto stream_type = f.get_profile().stream_type();
            auto stream_index = f.get_profile().stream_index();
            auto stream_format = f.get_profile().format();

            printf("Frameset contain (%s, %d, %s) frame. frame_number: %llu ; frame_TS: %f ; ros_TS(NSec)",
                rs2_stream_to_string(stream_type), stream_index, rs2_format_to_string(stream_format), f.get_frame_number(), f.get_timestamp());
            if (f.is<rs2::video_frame>())
                std::cout << "frame: " << f.as<rs2::video_frame>().get_width() << " x " << f.as<rs2::video_frame>().get_height() << std::endl;
        }
#endif
        if (display_output_images)
        {
            rs2::frame depth = fset.first_or_default(RS2_STREAM_DEPTH);
            rs2::frame color = fset.first_or_default(RS2_STREAM_COLOR);
            if (depth)
                _frames_map[DEPTH_UNITED].first = depth.apply_filter(_colorizer);

            if (color)
                _frames_map[COLOR_UNITED].first = color;
        }
        if (display_output_images || display_original_images)
        {
            app.show(_frames_map);

            // Display titles and buttons:
            ImGui_ImplGlfw_NewFrame(1);
            
            RecordButton({ app.width(), app.height() });
            SaveFramesButton(frames_sets, { app.width(), app.height() });
            DrawTitles(fps, { app.width(), app.height() });

            ImGui::Render();
        }
    }

}

int main(int argc, char* argv[]) try
{
    rs2::log_to_file(RS2_LOG_SEVERITY_DEBUG, "C:\\projects\\librealsense\\build\\Debug\\lrs_log.txt");

    std::string working_dir, calibration_file;
    for (int c = 1; c < argc; ++c) {
        if (!std::strcmp(argv[c], "-h") || !std::strcmp(argv[c], "--help")) {
            std::cout << "USAGE: " << std::endl;
            std::cout << argv[0] << " <working_directory>" << std::endl;
            std::cout << std::endl;
            std::cout << "Connect exactly 2 RS devices." << std::endl;
            std::cout << "For every camera, a file should be located in <working_directory> named <serial_no>.cfg" << std::endl;
            std::cout << std::endl;
            std::cout << "The <serial_no>.cfg file should contain the following:" << std::endl;
            std::cout << "DEPTH, WIDTH1, HEIGHT1, FPS1, FORMAT1, STREAM_INDEX1" << std::endl;
            std::cout << "COLOR, WIDTH2, HEIGHT2, FPS2, FORMAT2, STREAM_INDEX2 " << std::endl;
            std::cout << std::endl;
            std::cout << "In addition, there will be another transformations.cfg file, of the following format:" << std::endl;
            std::cout << "<from_serial>, <to_serial>, <t1, t2, t3, ...., t12>" << std::endl;
            std::cout << "The t1, t2,... t12 line represents transformation from 1 device to the other." << std::endl;
            std::cout << std::endl;
            return -1;
        }
    }
    if (argc > 2)
    {
        working_dir = argv[1];
        calibration_file = argv[2];
    }
    else
    {
        working_dir = ".";
        calibration_file = "calibration_15.cfg";
    }

    CPointcloudStitcher pc_stitcher(working_dir, calibration_file);
    if (!pc_stitcher.Init())
        return -1;
    if (!pc_stitcher.Start())
        return -1;

    unsigned tiles_in_row = 4;
    unsigned tiles_in_col = 3;

    window app(1920, 1100, "RealSense Pointcloud-Stitching Example", tiles_in_row, tiles_in_col);
    //window app(1280, 720, "RealSense Pointcloud-Stitching Example", tiles_in_row, tiles_in_col);
    ImGui_ImplGlfw_Init(app, false);

    pc_stitcher.Run(app);
    pc_stitcher.StopSensors();
    pc_stitcher.CloseSensors();

    return EXIT_SUCCESS;
}
catch (const rs2::error& e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}

