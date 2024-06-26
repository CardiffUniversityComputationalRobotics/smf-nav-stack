/*! \file octomap_laser_scan.cpp
 * \brief Merge data from different laser_scan messages to incrementally build
 * an WorldModeler.
 *
 * \date November 07, 2022
 * \author Juan David Hernandez Vega, HernandezVegaJ@cardiff.ac.uk
 * \author Steven Alexander Silva Mendoza, silvas1@cardiff.ac.uk
 *
 * \details Purpose: Merge data from different laser_scan messages to
 * incrementally build an Octomap.
 *  Based on laser_octomap (Guillem Vallicrosa & J.D. Hernandez Vega, University
 * of Girona)
 */

#include <world_modeler.h>

//!  Constructor.
/*!
 * Load map parameters.
 * Subscribers to odometry and laser scan
 * Publishers to visualize the WorldModeler.
 */
WorldModeler::WorldModeler()
    : nh_(),
      local_nh_("~"),
      fixed_frame_("/fixed_frame"),
      robot_frame_("/robot_frame"),
      map_frame_("/map"),
      odometry_topic_("/odometry_topic"),
      offline_octomap_path_(""),
      octree_(NULL),
      octree_resol_(1.0),
      mapping_max_range_(5),
      initialized_(false),
      visualize_free_space_(false),
      rviz_timer_(0.0),
      robot_distance_view_max_(6.0),
      robot_distance_view_min_(1.5),
      robot_angle_view_(1.57),
      robot_velocity_threshold_(0.3),
      social_agent_radius_(0.4),
      social_agents_topic_("/pedsim_simulator/simulated_agents"),
      social_relevance_validity_checking_(false),
      social_heatmap_decay_factor_(65),
      min_z_pc_(0.05),
      max_z_pc_(1.0),
      social_comfort_amplitude_(6)
{
    //=======================================================================
    // Get parameters
    //=======================================================================
    local_nh_.param("resolution", octree_resol_, octree_resol_);
    local_nh_.param("map_frame", map_frame_, map_frame_);
    local_nh_.param("fixed_frame", fixed_frame_, fixed_frame_);
    local_nh_.param("robot_frame", robot_frame_, robot_frame_);
    local_nh_.param("offline_octomap_path", offline_octomap_path_,
                    offline_octomap_path_);
    local_nh_.param("visualize_free_space", visualize_free_space_,
                    visualize_free_space_);
    local_nh_.param("odometry_topic", odometry_topic_, odometry_topic_);
    local_nh_.param("rviz_timer", rviz_timer_, rviz_timer_);
    local_nh_.param("point_cloud_topic", point_cloud_topic_,
                    point_cloud_topic_);
    local_nh_.param("point_cloud_frame", point_cloud_frame_,
                    point_cloud_frame_);
    local_nh_.param("mapping_max_range", mapping_max_range_,
                    mapping_max_range_);
    local_nh_.param("robot_distance_view_max", robot_distance_view_max_, robot_distance_view_max_);
    local_nh_.param("robot_distance_view_min", robot_distance_view_min_, robot_distance_view_min_);
    local_nh_.param("robot_angle_view", robot_angle_view_, robot_angle_view_);
    local_nh_.param("robot_velocity_threshold", robot_velocity_threshold_, robot_velocity_threshold_);
    local_nh_.param("social_agent_radius", social_agent_radius_, social_agent_radius_);
    local_nh_.param("social_agents_topic", social_agents_topic_, social_agents_topic_);
    local_nh_.param("social_relevance_validity_checking", social_relevance_validity_checking_, social_relevance_validity_checking_);
    local_nh_.param("social_heatmap_decay_factor", social_heatmap_decay_factor_,
                    social_heatmap_decay_factor_);
    local_nh_.param("min_z_pc", min_z_pc_, min_z_pc_);
    local_nh_.param("max_z_pc", max_z_pc_, max_z_pc_);
    local_nh_.param("social_comfort_amplitude", social_comfort_amplitude_, social_comfort_amplitude_);

    // Transforms TF and catch the static transform from vehicle to laser_scan
    // sensor
    // tf_listener_.setExtrapolationLimit(ros::Duration(0.2));
    int count(0);
    ros::Time t;
    tf::StampedTransform transform;
    std::string err = "";

    // Get the corresponding tf
    count = 0;
    err = "cannot find tf from " + robot_frame_ + "to " +
          point_cloud_frame_;

    tf_listener_.getLatestCommonTime(robot_frame_, point_cloud_frame_, t,
                                     &err);

    initialized_ = false;
    do
    {
        try
        {
            tf_listener_.lookupTransform(
                robot_frame_, point_cloud_frame_, t,
                transform);
            initialized_ = true;
        }
        catch (std::exception e)
        {
            tf_listener_.waitForTransform(robot_frame_, point_cloud_frame_,
                                          ros::Time::now(), ros::Duration(1.0));
            tf_listener_.getLatestCommonTime(robot_frame_,
                                             point_cloud_frame_, t, &err);
            count++;
            ROS_WARN("%s:\n\tCannot find tf from %s to %s\n",
                     ros::this_node::getName().c_str(), robot_frame_.c_str(),
                     point_cloud_frame_.c_str());
        }
        if (count > 10)
        {
            ROS_ERROR("%s\n\tNo transform found. Aborting...",
                      ros::this_node::getName().c_str());
            exit(-1);
        }
    } while (ros::ok() && !initialized_);
    ROS_WARN("%s:\n\ttf from %s to %s OK\n",
             ros::this_node::getName().c_str(), robot_frame_.c_str(),
             point_cloud_frame_.c_str());

    //=======================================================================
    // Octree
    //=======================================================================
    if (offline_octomap_path_.size() > 0)
        octree_ = new octomap::OcTree(offline_octomap_path_);
    else
        octree_ = new octomap::OcTree(octree_resol_);
    ROS_WARN("%s:\n\tLoaded\n", ros::this_node::getName().c_str());
    octree_->setProbHit(0.7);
    octree_->setProbMiss(0.4);
    octree_->setClampingThresMin(0.1192);
    octree_->setClampingThresMax(0.971);

    //=======================================================================
    // Gridmap
    //=======================================================================

    grid_map_.setFrameId(map_frame_);
    grid_map_.add("obstacle");
    grid_map_.add("full");
    grid_map_.add("comfort");
    grid_map_.add("social_heatmap");
    grid_map_.setGeometry(grid_map::Length(1, 1), octree_resol_);

    // SOCIAL HEATMAP
    social_heatmap_.setTimeDecayFactor(social_heatmap_decay_factor_);

    //=======================================================================
    // Publishers
    //=======================================================================
    octomap_marker_pub_ = local_nh_.advertise<visualization_msgs::MarkerArray>(
        "octomap_map", 2, true);
    grid_map_pub_ = local_nh_.advertise<grid_map_msgs::GridMap>("social_grid_map", 1, true);
    relevant_agents_pub_ = local_nh_.advertise<pedsim_msgs::AgentStates>("relevant_agents", 1, true);

    //=======================================================================
    // Subscribers
    //=======================================================================

    // Agent states callback
    agent_states_sub_ = nh_.subscribe(social_agents_topic_, 1,
                                      &WorldModeler::agentStatesCallback, this);

    // Odometry data (feedback)
    odom_sub_ =
        nh_.subscribe(odometry_topic_, 1, &WorldModeler::odomCallback, this);
    nav_sts_available_ = false;
    if (!nav_sts_available_)
        ROS_WARN("%s:\n\tWaiting for odometry\n",
                 ros::this_node::getName().c_str());
    ros::Rate loop_rate(10);
    while (ros::ok() && !nav_sts_available_)
    {
        ros::spinOnce();
        loop_rate.sleep();
    }
    ROS_WARN("%s:\n\tOdometry received\n", ros::this_node::getName().c_str());

    if (offline_octomap_path_.size() == 0)
    {
        // POINTCLOUD
        std::vector<std::string> pc_need_frames;
        pc_need_frames.push_back(point_cloud_frame_);
        pc_need_frames.push_back(fixed_frame_);
        pc_need_frames.push_back(robot_frame_);
        point_cloud_sub_.subscribe(nh_, point_cloud_topic_, 1);
        point_cloud_mn_.reset(new tf::MessageFilter<sensor_msgs::PointCloud2>(point_cloud_sub_, tf_listener_, "", 1));
        point_cloud_mn_->setTargetFrames(pc_need_frames);
        point_cloud_mn_->registerCallback(boost::bind(&WorldModeler::pointCloudCallback, this, _1));
    }

    //=======================================================================
    // Services
    //=======================================================================
    save_binary_octomap_srv_ = local_nh_.advertiseService(
        "save_binary", &WorldModeler::saveBinaryOctomapSrv, this);
    save_full_octomap_srv_ = local_nh_.advertiseService(
        "save_full", &WorldModeler::saveFullOctomapSrv, this);
    get_binary_octomap_srv_ = local_nh_.advertiseService(
        "get_binary", &WorldModeler::getBinaryOctomapSrv, this);
    get_grid_map_srv_ = local_nh_.advertiseService(
        "get_grid_map", &WorldModeler::getGridMapSrv, this);

    // Timer for publishing
    if (rviz_timer_ > 0.0)
    {
        timer_ = nh_.createTimer(ros::Duration(rviz_timer_),
                                 &WorldModeler::timerCallback, this);
    }
}

//! Destructor.
WorldModeler::~WorldModeler()
{
    ROS_INFO("%s:\n\tOctree has been deleted\n",
             ros::this_node::getName().c_str());
    delete octree_;
}

//! LaserScan callback.
/*!
 * Callback for receiving the laser scan data (taken from octomap_server)
 */
void WorldModeler::pointCloudCallback(
    const sensor_msgs::PointCloud2::ConstPtr &cloud)
{

    // ROS_INFO_STREAM("PROCESSING POINTCLOUD");
    //
    // ground filtering in base frame
    //
    PCLPointCloud pc; // input cloud for filtering and ground-detection
    pcl::fromROSMsg(*cloud, pc);

    float minX = -0.6, minY = -0.6, minZ = -12;
    float maxX = 0.6, maxY = 0.6, maxZ = 12;

    // ROS_INFO_STREAM("ABOUT TO PROCESS AGENTS");

    if (social_agents_in_radius_.agent_states.size() > 0)
    {
        for (int i = 0; i < social_agents_in_radius_.agent_states.size(); i++)
        {

            std::string err = "cannot find transform from agent to camera frame";

            tf::StampedTransform transform;
            ros::Time t;

            try
            {
                tf_listener_.getLatestCommonTime(cloud->header.frame_id, "agent_" + std::to_string(social_agents_in_radius_.agent_states[i].id), t,
                                                 &err);
                tf_listener_.lookupTransform(cloud->header.frame_id, "agent_" + std::to_string(social_agents_in_radius_.agent_states[i].id),
                                             t, transform);

                tf::Quaternion q(
                    transform.getRotation().x(),
                    transform.getRotation().y(),
                    transform.getRotation().z(),
                    transform.getRotation().w());
                tf::Matrix3x3 m(q);
                double roll, pitch, yaw;
                m.getRPY(roll, pitch, yaw);

                // Z -> X
                // X -> Y
                // Y -> -Z
                pcl::CropBox<pcl::PointXYZ> boxFilter;
                boxFilter.setMin(Eigen::Vector4f(minX, minY, minZ, 0));
                boxFilter.setMax(Eigen::Vector4f(maxX, maxY, maxZ, 0));
                boxFilter.setInputCloud(pc.makeShared());
                boxFilter.setTranslation(Eigen::Vector3f(transform.getOrigin().x(), transform.getOrigin().y(), transform.getOrigin().z()));
                boxFilter.setRotation(Eigen::Vector3f(roll, pitch, yaw));
                boxFilter.setNegative(true);
                boxFilter.filter(pc);
            }
            catch (tf::TransformException &ex)
            {

                ROS_ERROR_STREAM("Transform error of sensor data: "
                                 << ex.what() << ", quitting callback");
                return;
            }
        }
    }
    // ROS_INFO_STREAM("ABOUT TO PROCESS AGENTS FINISHED");

    ros::Time t;
    std::string err = "cannot find transform from robot_frame to scan frame";

    tf::StampedTransform sensorToWorldTf;
    try
    {
        tf_listener_.getLatestCommonTime(fixed_frame_, cloud->header.frame_id, t,
                                         &err);
        //        tf_listener_.lookupTransform(robot_frame_,
        //        laser_scan_msg->header.frame_id, t,
        //                                     tf_robot_to_laser_scan);

        tf_listener_.lookupTransform(fixed_frame_, cloud->header.frame_id, cloud->header.stamp,
                                     sensorToWorldTf);
    }
    catch (tf::TransformException &ex)
    {
        ROS_ERROR_STREAM("Transform error of sensor data: "
                         << ex.what() << ", quitting callback");
        return;
    }

    Eigen::Matrix4f sensorToWorld;
    pcl_ros::transformAsMatrix(sensorToWorldTf, sensorToWorld);

    // set up filter for height range, also removes NANs:
    // pcl::PassThrough<PCLPoint> pass_x;
    // pass_x.setFilterFieldName("x");
    // pass_x.setFilterLimits(0.15, 4.0);
    // pcl::PassThrough<PCLPoint> pass_y;
    // pass_y.setFilterFieldName("y");
    // pass_y.setFilterLimits(0.15, 4.0);
    pcl::PassThrough<PCLPoint> pass_z;
    pass_z.setFilterFieldName("z");
    pass_z.setFilterLimits(min_z_pc_, max_z_pc_); // TODO

    PCLPointCloud pc_ground;    // segmented ground plane
    PCLPointCloud pc_nonground; // everything else

    // directly transform to map frame:
    pcl::transformPointCloud(pc, pc, sensorToWorld);

    // just filter height range:
    // pass_x.setInputCloud(pc.makeShared());
    // pass_x.filter(pc);
    // pass_y.setInputCloud(pc.makeShared());
    // pass_y.filter(pc);
    pass_z.setInputCloud(pc.makeShared());
    pass_z.filter(pc);

    pc_nonground = pc;

    // pc_nonground is empty without ground segmentation
    pc_ground.header = pc.header;
    pc_nonground.header = pc.header;

    // ROS_INFO_STREAM("INSERT SCAN START");

    insertScan(sensorToWorldTf.getOrigin(), pc_ground, pc_nonground);
    //
    //    double total_elapsed = (ros::WallTime::now() - startTime).toSec();
    //    ROS_DEBUG("Pointcloud insertion in OctomapServer done (%zu+%zu pts
    //    (ground/nonground), %f sec)",
    //              pc_ground.size(), pc_nonground.size(), total_elapsed);
    //
    //    publishAll(cloud->header.stamp);

    defineSocialGridMap();
}

void WorldModeler::insertScan(const tf::Point &sensorOriginTf,
                              const PCLPointCloud &ground,
                              const PCLPointCloud &nonground)
{
    octomap::point3d sensorOrigin = octomap::pointTfToOctomap(sensorOriginTf);

    if (!octree_->coordToKeyChecked(sensorOrigin, m_updateBBXMin) ||
        !octree_->coordToKeyChecked(sensorOrigin, m_updateBBXMax))
    {
        ROS_ERROR_STREAM("Could not generate Key for origin " << sensorOrigin);
    }

#ifdef COLOR_OCTOMAP_SERVER
    unsigned char *colors = new unsigned char[3];
#endif

    // instead of direct scan insertion, compute update to filter ground:
    octomap::KeySet free_cells, occupied_cells;

    // all other points: free on ray, occupied on endpoint:
    double m_maxRange(mapping_max_range_); // TODO
    for (PCLPointCloud::const_iterator it = nonground.begin();
         it != nonground.end(); ++it)
    {
        octomap::point3d point(it->x, it->y, it->z); // TODO
        // maxrange check
        if ((m_maxRange < 0.0) || ((point - sensorOrigin).norm() <= m_maxRange))
        {
            // free cells
            if (octree_->computeRayKeys(sensorOrigin, point, m_keyRay))
            {
                free_cells.insert(m_keyRay.begin(), m_keyRay.end());
            }
            // occupied endpoint
            octomap::OcTreeKey key;
            if (octree_->coordToKeyChecked(point, key))
            {
                occupied_cells.insert(key);

                updateMinKey(key, m_updateBBXMin);
                updateMaxKey(key, m_updateBBXMax);
            }
        }
        else
        {
            // ray longer than maxrange:;
            octomap::point3d new_end =
                sensorOrigin + (point - sensorOrigin).normalized() * m_maxRange;
            if (octree_->computeRayKeys(sensorOrigin, new_end, m_keyRay))
            {
                free_cells.insert(m_keyRay.begin(), m_keyRay.end());

                octomap::OcTreeKey endKey;
                if (octree_->coordToKeyChecked(new_end, endKey))
                {
                    free_cells.insert(endKey);
                    updateMinKey(endKey, m_updateBBXMin);
                    updateMaxKey(endKey, m_updateBBXMax);
                }
                else
                {
                    ROS_ERROR_STREAM("Could not generate Key for endpoint " << new_end);
                }
            }
        }
    }

    // mark free cells only if not seen occupied in this cloud
    for (octomap::KeySet::iterator it = free_cells.begin(),
                                   end = free_cells.end();
         it != end; ++it)
    {
        if (occupied_cells.find(*it) == occupied_cells.end())
        {
            octree_->updateNode(*it, false);
        }
    }

    // now mark all occupied cells:
    for (octomap::KeySet::iterator it = occupied_cells.begin(),
                                   end = occupied_cells.end();
         it != end; it++)
    {
        octree_->updateNode(*it, true);
    }
}

//! Odometry callback.
/*!
 * Callback for getting updated vehicle odometry.
 */
void WorldModeler::odomCallback(const nav_msgs::OdometryConstPtr &odom_msg)
{
    if (!nav_sts_available_)
        nav_sts_available_ = true;

    robot_odometry_ = odom_msg;
}

//! Agent States callback.
/*!
 * Callback for getting updated agent states.
 */
void WorldModeler::agentStatesCallback(const pedsim_msgs::AgentStatesConstPtr &agent_states_msg)
{

    if (nav_sts_available_)
    {
        agent_states_ = agent_states_msg;

        std::vector<pedsim_msgs::AgentState> agent_state_vector;

        social_agents_in_radius_vector_.clear();

        double robot_velocity =
            std::sqrt(std::pow(robot_odometry_->twist.twist.linear.x, 2) + std::pow(robot_odometry_->twist.twist.linear.y, 2));

        actual_fov_distance_ = robot_distance_view_max_ / robot_velocity_threshold_ * robot_velocity;

        if (actual_fov_distance_ < robot_distance_view_min_)
        {
            actual_fov_distance_ = robot_distance_view_min_;
        }
        else if (actual_fov_distance_ > robot_distance_view_max_)
        {
            actual_fov_distance_ = robot_distance_view_max_;
        }

        for (int i = 0; i < agent_states_msg->agent_states.size(); i++)
        {
            if (this->isAgentInRFOV(agent_states_msg->agent_states[i]))
            {
                agent_state_vector.push_back(agent_states_msg->agent_states[i]);
            }
        }

        relevant_agent_states_.agent_states = agent_state_vector;

        social_agents_in_radius_.agent_states = social_agents_in_radius_vector_;

        relevant_agent_states_.header.stamp = ros::Time::now();
        relevant_agent_states_.header.frame_id = map_frame_;
    }

    relevant_agents_pub_.publish(relevant_agent_states_);
}

bool WorldModeler::isAgentInRFOV(const pedsim_msgs::AgentState agent_state)
{
    if (!social_relevance_validity_checking_)
    {
        return true;
    }

    double d_robot_agent = std::sqrt(std::pow(agent_state.pose.position.x - robot_odometry_->pose.pose.position.x, 2) +
                                     std::pow(agent_state.pose.position.y - robot_odometry_->pose.pose.position.y, 2));

    if (d_robot_agent < mapping_max_range_ + 2)
    {
        social_agents_in_radius_vector_.push_back(agent_state);
    }

    if (d_robot_agent > actual_fov_distance_)
    {
        return false;
    }

    double tetha_robot_agent = atan2((agent_state.pose.position.y - robot_odometry_->pose.pose.position.y),
                                     (agent_state.pose.position.x - robot_odometry_->pose.pose.position.x));

    if (tetha_robot_agent < 0)
    {
        tetha_robot_agent = 2 * M_PI + tetha_robot_agent;
    }

    tf::Quaternion q(robot_odometry_->pose.pose.orientation.x, robot_odometry_->pose.pose.orientation.y,
                     robot_odometry_->pose.pose.orientation.z, robot_odometry_->pose.pose.orientation.w);

    tf::Matrix3x3 m(q);
    double roll, pitch, yaw;
    m.getRPY(roll, pitch, yaw);

    double robot_angle = yaw;

    if (robot_angle < 0)
    {
        robot_angle = 2 * M_PI + robot_angle;
    }

    if (tetha_robot_agent > (robot_angle + M_PI))
        tetha_robot_agent = abs(robot_angle + 2 * M_PI - tetha_robot_agent);
    else if (robot_angle > (tetha_robot_agent + M_PI))
        tetha_robot_agent = abs(tetha_robot_agent + 2 * M_PI - robot_angle);
    else
        tetha_robot_agent = abs(tetha_robot_agent - robot_angle);

    return abs(tetha_robot_agent) < robot_angle_view_;
}

bool WorldModeler::isRobotInFront(pedsim_msgs::AgentState agent_state, grid_map::Position position)

{

    double tetha_agent_robot = atan2((position[1] - agent_state.pose.position.y),
                                     (position[0] - agent_state.pose.position.x));
    if (tetha_agent_robot < 0)
    {
        tetha_agent_robot = 2 * M_PI + tetha_agent_robot;
    }

    tf::Quaternion q(agent_state.pose.orientation.x, agent_state.pose.orientation.y,
                     agent_state.pose.orientation.z, agent_state.pose.orientation.w);

    tf::Matrix3x3 m(q);
    double roll, pitch, yaw;
    m.getRPY(roll, pitch, yaw);

    double agent_angle = yaw;

    if (agent_angle < 0)
    {
        agent_angle = 2 * M_PI + agent_angle;
    }

    if (tetha_agent_robot > (agent_angle + M_PI))
        tetha_agent_robot = abs(agent_angle + 2 * M_PI - tetha_agent_robot);
    else if (agent_angle > (tetha_agent_robot + M_PI))
        tetha_agent_robot = abs(tetha_agent_robot + 2 * M_PI - agent_angle);
    else
        tetha_agent_robot = abs(tetha_agent_robot - agent_angle);

    if (abs(tetha_agent_robot) < robot_angle_view_)
        return true;

    return false;
}

// !CALCULATE COMFORT AT AN SPECIFIC POSITION IN THE GRIDMAP

double WorldModeler::getExtendedPersonalSpace(pedsim_msgs::AgentState agent_state, grid_map::Position position)
{

    double distance_robot_agent = std::sqrt(std::pow(agent_state.pose.position.x - position[0], 2) +
                                            std::pow(agent_state.pose.position.y - position[1], 2));

    double tetha_robot_agent = atan2((position[1] - agent_state.pose.position.y),
                                     (position[0] - agent_state.pose.position.x));

    if (tetha_robot_agent < 0)
    {
        tetha_robot_agent = 2 * M_PI + tetha_robot_agent;
    }

    double tetha_orientation;
    if (abs(agent_state.twist.linear.x) > 0 || abs(agent_state.twist.linear.y) > 0)
    {
        tetha_orientation = atan2(agent_state.twist.linear.y, agent_state.twist.linear.x);
    }
    else
    {
        tf::Quaternion q(agent_state.pose.orientation.x, agent_state.pose.orientation.y,
                         agent_state.pose.orientation.z, agent_state.pose.orientation.w);

        tf::Matrix3x3 m(q);
        double roll, pitch, yaw;
        m.getRPY(roll, pitch, yaw);

        tetha_orientation = yaw;
    }

    if (tetha_orientation < 0)
    {
        tetha_orientation = 2 * M_PI + tetha_orientation;
    }

    bool robot_in_front = false;
    bool robot_in_fov = false;
    double mod_sigma_y;
    double agent_velocity;

    agent_velocity =
        std::sqrt(std::pow(agent_state.twist.linear.x, 2) + std::pow(agent_state.twist.linear.y, 2));

    robot_in_front = this->isRobotInFront(agent_state, position);

    if (robot_in_front)
    {
        if (robot_in_fov)
            mod_sigma_y = (1 + agent_velocity * fv + fFront + fFieldOfView) * sigma_y;
        else
            mod_sigma_y = (1 + agent_velocity * fv + fFront) * sigma_y;
    }
    else
    {
        mod_sigma_y = sigma_y;
    }

    double basic_personal_space_value =
        social_comfort_amplitude_ *
        std::exp(-(
            std::pow(distance_robot_agent * std::cos(tetha_robot_agent - tetha_orientation) / (std::sqrt(2) * sigma_x),
                     2) +
            std::pow(distance_robot_agent * std::sin(tetha_robot_agent - tetha_orientation) / (std::sqrt(2) * mod_sigma_y),
                     2)));

    return basic_personal_space_value;
}

//! Time callback.
/*!
 * Callback for publishing the map periodically using the WorldModeler RViz plugin.
 */
void WorldModeler::timerCallback(const ros::TimerEvent &e)
{

    if (offline_octomap_path_.size() != 0)
    {
        defineSocialGridMap();
    }

    // Declare message
    octomap_msgs::Octomap msg;
    octomap_msgs::binaryMapToMsg(*octree_, msg);
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = fixed_frame_;
    if (visualize_free_space_)
    {
        publishMap();

        grid_map_.setTimestamp(ros::Time::now().toNSec());
        grid_map_msgs::GridMap message;
        grid_map::GridMapRosConverter::toMessage(grid_map_, message);

        grid_map_pub_.publish(message);
    }
}

//! Save binary service
/*!
 * Service for saving the binary Octomap into the home folder
 */
bool WorldModeler::saveBinaryOctomapSrv(std_srvs::Empty::Request &req,
                                        std_srvs::Empty::Response &res)
{
    // Saves current octree_ in home folder
    std::string fpath(getenv("HOME"));
    octree_->writeBinary(fpath + "/map_laser_octomap.bt");
    return true;
}

//! Save binary service
/*!
 * Service for saving the full Octomap into the home folder
 */
bool WorldModeler::saveFullOctomapSrv(std_srvs::Empty::Request &req,
                                      std_srvs::Empty::Response &res)
{
    // Saves current octree_ in home folder (full probabilities)
    std::string fpath(getenv("HOME"));
    octree_->write(fpath + "/map_laser_octomap.ot");
    return true;
}

//! Get binary service
/*!
 * Service for getting the binary Octomap
 */
bool WorldModeler::getBinaryOctomapSrv(OctomapSrv::Request &req,
                                       OctomapSrv::GetOctomap::Response &res)
{
    ROS_INFO("%s:\n\tSending binary map data on service request\n",
             ros::this_node::getName().c_str());
    res.map.header.frame_id = fixed_frame_;
    res.map.header.stamp = ros::Time::now();

    if (!octomap_msgs::binaryMapToMsg(*octree_, res.map))
        return false;

    return true;
}

//! Get binary service
/*!
 * Service for getting the binary Octomap
 */
bool WorldModeler::getGridMapSrv(grid_map_msgs::GetGridMap::Request &req,
                                 grid_map_msgs::GetGridMap::Response &res)
{
    ROS_INFO("%s:\n\tSending grid map data on service\n",
             ros::this_node::getName().c_str());

    grid_map_.setTimestamp(ros::Time::now().toNSec());
    grid_map::GridMapRosConverter::toMessage(grid_map_, res.map);

    return true;
}

//! Publish the map
/*!
 * Service for saving the binary of the Octomap into the home folder
 */
void WorldModeler::publishMap()
{
    // Declare message and resize
    visualization_msgs::MarkerArray occupiedNodesVis;
    occupiedNodesVis.markers.resize(octree_->getTreeDepth() + 1);

    // Octree limits for height map
    double min_x, min_y, max_x, max_y, min_z_, max_z_;
    octree_->getMetricMin(min_x, min_y, min_z_);
    octree_->getMetricMax(max_x, max_y, max_z_);

    // Traverse all leafs in the tree:
    for (octomap::OcTree::iterator it = octree_->begin(octree_->getTreeDepth()),
                                   end = octree_->end();
         it != end; ++it)
    {
        if (octree_->isNodeOccupied(*it))
        {
            double x = it.getX();
            double y = it.getY();
            double z = it.getZ();

            // Create marker:
            unsigned idx = it.getDepth();
            assert(idx < occupiedNodesVis.markers.size());

            geometry_msgs::Point cubeCenter;
            cubeCenter.x = x;
            cubeCenter.y = y;
            cubeCenter.z = z;

            occupiedNodesVis.markers[idx].points.push_back(cubeCenter);
        }

        if (!ros::ok())
            break;
    }
    // Finish Headers and options
    for (unsigned i = 0; i < occupiedNodesVis.markers.size(); ++i)
    {
        double size = octree_->getNodeSize(i);

        occupiedNodesVis.markers[i].header.frame_id = fixed_frame_;
        occupiedNodesVis.markers[i].header.stamp = ros::Time::now();
        occupiedNodesVis.markers[i].ns = fixed_frame_;
        occupiedNodesVis.markers[i].id = i;
        occupiedNodesVis.markers[i].type = visualization_msgs::Marker::CUBE_LIST;
        occupiedNodesVis.markers[i].scale.x = size;
        occupiedNodesVis.markers[i].scale.y = size;
        occupiedNodesVis.markers[i].scale.z = size;
        occupiedNodesVis.markers[i].color.r = 0.5;
        occupiedNodesVis.markers[i].color.g = 0.5;
        occupiedNodesVis.markers[i].color.b = 0.5;
        occupiedNodesVis.markers[i].color.a = 1.0;

        if (occupiedNodesVis.markers[i].points.size() > 0)
            occupiedNodesVis.markers[i].action = visualization_msgs::Marker::ADD;
        else
            occupiedNodesVis.markers[i].action = visualization_msgs::Marker::DELETE;

        if (!ros::ok())
            break;
    }

    // Publish it
    octomap_marker_pub_.publish(occupiedNodesVis);
}

void WorldModeler::defineSocialGridMap()
{
    // ! OCTOMAP PREPARATION

    grid_map::Position3 min_bound;
    grid_map::Position3 max_bound;

    grid_map_.clearAll();

    octree_->getMetricMin(min_bound(0), min_bound(1), min_bound(2));
    octree_->getMetricMax(max_bound(0), max_bound(1), max_bound(2));

    grid_map::GridMapOctomapConverter::fromOctomap(*octree_, "obstacles", grid_map_, &min_bound, &max_bound);
    grid_map_["obstacles"] = 150 * grid_map_["obstacles"];
    grid_map_["full"] = 1.0 * grid_map_["obstacles"];

    grid_map::Matrix &full_grid_map = grid_map_["full"];
    grid_map::Matrix &comfort_grid_map = grid_map_["comfort"];

    // !SOCIAL AGENTS GRID MAP PREPARATION

    for (int i = 0; i < relevant_agent_states_.agent_states.size(); i++)
    {
        grid_map::Position center(relevant_agent_states_.agent_states[i].pose.position.x, relevant_agent_states_.agent_states[i].pose.position.y);

        for (grid_map::CircleIterator iterator(grid_map_, center, social_agent_radius_);
             !iterator.isPastEnd(); ++iterator)
        {
            try
            {
                grid_map::Index index(*iterator);
                full_grid_map(index(0), index(1)) = 100;
            }
            catch (const std::out_of_range &oor)
            {
                ROS_ERROR("TRIED TO DEFINE AN AGENT OUT OF RANGE");
            }
        }

        for (grid_map::CircleIterator iterator(grid_map_, center, 2.25);
             !iterator.isPastEnd(); ++iterator)
        {
            try
            {

                grid_map::Position temp_pos;

                grid_map_.getPosition(*iterator, temp_pos);

                grid_map::Index index(*iterator);

                double last_val = comfort_grid_map(index(0), index(1));

                if (isnan(last_val))
                {
                    last_val = getExtendedPersonalSpace(relevant_agent_states_.agent_states[i], temp_pos);
                }
                else
                {
                    last_val += getExtendedPersonalSpace(relevant_agent_states_.agent_states[i], temp_pos);
                }

                comfort_grid_map(index(0), index(1)) = last_val;
            }
            catch (const std::out_of_range &oor)
            {
                ROS_ERROR("TRIED TO DEFINE COMFORT OUT OF RANGE");
            }
        }
    }

    social_heatmap_.updateSocialHeatmap(grid_map_, relevant_agent_states_);

    grid_map_["full"] = full_grid_map;
    grid_map_["comfort"] = comfort_grid_map;
    grid_map_["social_heatmap"] = social_heatmap_.getSocialHeatmap();
}

//! Main function
int main(int argc, char **argv)
{
    //=======================================================================
    // Override SIGINT handler
    //=======================================================================
    signal(SIGINT, stopNode);

    // Init ROS node
    ros::init(argc, argv, "octomap_laser_scan");
    ros::NodeHandle private_nh("~");

    // Constructor
    WorldModeler mapper;

    // Spin
    ros::spin();

    // Exit main function without errors
    return 0;
}
