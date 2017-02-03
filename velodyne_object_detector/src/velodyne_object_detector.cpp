#include <velodyne_object_detector/velodyne_object_detector.h>

namespace velodyne_object_detector
{

VelodyneObjectDetector::VelodyneObjectDetector()
: m_nh("~")
 , PUCK_NUM_RINGS(16)
 , ANGLE_BETWEEN_SCANPOINTS(0.2f) // 0.1 for 5Hz 0.2 for 10Hz 0.4 for 20Hz
 , m_max_prob_by_distance(0.75f)
 , m_max_intensity_range(100.f)
 , m_certainty_threshold("certainty_threshold", 0.0, 0.01, 1.0, 0.5)
 , m_dist_coeff("dist_coeff", 0.0, 0.1, 10.0, 1.0)
 , m_intensity_coeff("intensity_coeff", 0.0, 0.0001, 0.01, (1.f - m_max_prob_by_distance)/m_max_intensity_range)
 , m_weight_for_small_intensities("weight_for_small_intensities", 1.f, 1.f, 30.f, 11.f)
 , m_median_min_dist("median_min_dist", 0.0, 0.01, .2, 0.1)
 , m_median_thresh1_dist("median_thresh1_dist", 0.0, 0.05, 0.5, 0.25)
 , m_median_thresh2_dist("median_thresh2_dist", 0.0, 0.5, 30.0, 6.0)
 , m_median_max_dist("median_max_dist", 0.0, 0.5, 50.0, 8.0)
 , m_max_dist_for_median_computation("max_dist_for_median_computation", 1.0, 0.5, 10.0, 6.0)
 , m_object_width("object_width", 0.0, 0.01, 10.0, 0.5)
 , m_distance_to_comparison_points("distance_to_comparison_points", 0, 1, 100, 10)
 , m_points_topic("/velodyne_points")
{
   m_nh.getParam("points_topic", m_points_topic);
   m_velodyne_sub = m_nh.subscribe(m_points_topic, 1000, &VelodyneObjectDetector::velodyneCallback, this);

   m_pub = m_nh.advertise<InputPointCloud >("output", 1);
   m_pub_obstacle_cloud = m_nh.advertise<DebugOutputPointCloud >("obstacles", 1);
   m_pub_cluster_marker = m_nh.advertise<visualization_msgs::Marker>("cluster_marker",0 );

   m_nh.getParam("certainty_threshold_launch", m_certainty_threshold_launch);
   m_certainty_threshold.set(m_certainty_threshold_launch);

   m_certainty_threshold.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_dist_coeff.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_intensity_coeff.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_weight_for_small_intensities.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));

   m_median_min_dist.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_median_thresh1_dist.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_median_thresh2_dist.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_median_max_dist.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));

   m_max_dist_for_median_computation.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_object_width.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
   m_distance_to_comparison_points.setCallback(boost::bind(&VelodyneObjectDetector::nop, this));
}

// sort points by ring number and save indices in vector
void VelodyneObjectDetector::splitCloudByRing(InputPointCloud &cloud, std::vector<std::vector<unsigned int> > &clouds_per_ring)
{
   for(unsigned int point_index = 0; point_index < cloud.size(); point_index++)
   {
      clouds_per_ring[cloud.points[point_index].ring].push_back(point_index);
   }
}

void VelodyneObjectDetector::medianFilter(std::vector<float> &input,
                                          std::vector<float> &input_intensities,
                                          std::vector<float> &filtered_output,
                                          std::vector<float> &filtered_output_intensities,
                                          float object_width,
                                          float max_distance_difference)
{
   for(int point_index = 0; point_index < (int)input.size(); point_index++)
   {
      float alpha = static_cast<float>(std::atan((object_width/2.f)/input[point_index]) * 180.f / M_PI);
      int kernel_size = (int)std::ceil(alpha / ANGLE_BETWEEN_SCANPOINTS) + 1;

      ROS_DEBUG_STREAM_THROTTLE(0.1, "kernel_size " << kernel_size << " alpha " << alpha << " object_width " << object_width << " input[point_index] " << input[point_index]);

      int point_index_neighborhood_start = std::max(0, point_index - kernel_size);
      int point_index_neighborhood_end = std::min((int)input.size(), point_index + kernel_size);
      //std::vector<float> neighborhood_distances(input.begin() + point_index_neighborhood_start, input.begin() + point_index_neighborhood_end);
      std::vector<float> neighborhood_distances;
      std::vector<float> neighborhood_intensities;
      // get distances of neighbors
      if(max_distance_difference == 0.f)
      {
         // take all
         neighborhood_distances = std::vector<float> (input.begin() + point_index_neighborhood_start, input.begin() + point_index_neighborhood_end);
         neighborhood_intensities = std::vector<float> (input_intensities.begin() + point_index_neighborhood_start, input_intensities.begin() + point_index_neighborhood_end);
      }
      else
      {
         // filter if difference of distances of neighbor and the current point exceeds a threshold
         for(int local_point_index = point_index_neighborhood_start; local_point_index < point_index_neighborhood_end; local_point_index++)
         {
            float abs_distance_difference_to_current_point = fabs(input[point_index] - input[local_point_index]);
            if(abs_distance_difference_to_current_point < max_distance_difference)
            {
               neighborhood_distances.push_back(input[local_point_index]);
               neighborhood_intensities.push_back(input_intensities[local_point_index]);
            }
         }
      }
      // get median of neighborhood distances
      size_t middle = neighborhood_distances.size() / 2;
      std::nth_element(neighborhood_distances.begin(), neighborhood_distances.begin() + middle, neighborhood_distances.end());
      filtered_output[point_index] = neighborhood_distances[middle];
      std::nth_element(neighborhood_intensities.begin(), neighborhood_intensities.begin() + middle, neighborhood_intensities.end());
      filtered_output_intensities[point_index] = neighborhood_intensities[middle];
   }
}

void VelodyneObjectDetector::detectSegmentsMedian(InputPointCloud &cloud,
                                            std::vector<std::vector<unsigned int> > &clouds_per_ring)
{
   DebugOutputPointCloud obstacle_cloud;
   obstacle_cloud.header = cloud.header;

   for(unsigned int ring_index = 0; ring_index < clouds_per_ring.size(); ring_index++)
   {
      // median filter on distances
      std::vector<float> distances_ring(clouds_per_ring[ring_index].size(), 0.f);
      for(unsigned int point_index = 0; point_index < clouds_per_ring[ring_index].size(); point_index++)
      {
         unsigned int index_of_point_in_cloud = clouds_per_ring[ring_index][point_index];
         distances_ring[point_index] = cloud.points[index_of_point_in_cloud].getVector3fMap().norm();
      }

      // median filter on intensities
      std::vector<float> intensities_ring(clouds_per_ring[ring_index].size(), 0.f);
      for(unsigned int point_index = 0; point_index < clouds_per_ring[ring_index].size(); point_index++)
      {
         unsigned int index_of_point_in_cloud = clouds_per_ring[ring_index][point_index];
         intensities_ring[point_index] = cloud.points[index_of_point_in_cloud].intensity;
      }

      std::vector<float> distances_ring_filtered(distances_ring.size(), 0.f);
      std::vector<float> distances_ring_filtered_more(distances_ring.size(), 0.f);
      std::vector<float> intensities_ring_filtered(intensities_ring.size(), 0.f);
      std::vector<float> intensities_ring_filtered_more(intensities_ring.size(), 0.f);

      medianFilter(distances_ring, intensities_ring, distances_ring_filtered, intensities_ring_filtered, m_object_width(), m_max_dist_for_median_computation());
      medianFilter(distances_ring, intensities_ring, distances_ring_filtered_more, intensities_ring_filtered_more, m_object_width() * 4.f, m_max_dist_for_median_computation());

      for(int point_index = m_distance_to_comparison_points(); point_index < (int)clouds_per_ring[ring_index].size() - m_distance_to_comparison_points(); point_index++)
      {
         float certainty_value = 0.f;

         float difference_distances = -(distances_ring_filtered[point_index] * 2.f
                            - distances_ring_filtered_more[point_index - m_distance_to_comparison_points()]
                            - distances_ring_filtered_more[point_index + m_distance_to_comparison_points()]);

         float difference_intensities = intensities_ring_filtered[point_index] * 2.f
                                        - intensities_ring_filtered_more[point_index - m_distance_to_comparison_points()]
                                        - intensities_ring_filtered_more[point_index + m_distance_to_comparison_points()];
         // cap absolute difference to 0 - m_max_intensity_range
         // and do some kind of weighting, bigger weight -> bigger weight for smaller intensity differnces
         difference_intensities = static_cast<float>(fabs(difference_intensities));
         difference_intensities = std::min(difference_intensities, m_max_intensity_range/m_weight_for_small_intensities());
         difference_intensities *= m_weight_for_small_intensities();

         if(difference_distances < m_median_min_dist() || difference_distances > m_median_max_dist())
         {
            certainty_value = 0.f;
         }
         else{
            if(difference_distances >= m_median_min_dist() && difference_distances < m_median_thresh1_dist())
            {
               certainty_value = difference_distances * m_dist_coeff() * (m_max_prob_by_distance/m_median_thresh1_dist()) + difference_intensities * m_intensity_coeff();
            }
            if(difference_distances >= m_median_thresh1_dist() && difference_distances < m_median_thresh2_dist())
            {
               certainty_value = m_dist_coeff() * m_max_prob_by_distance + difference_intensities * m_intensity_coeff();
            }
            if(difference_distances >= m_median_thresh2_dist() && difference_distances < m_median_max_dist())
            {
               certainty_value = (m_max_prob_by_distance / (m_median_max_dist() - m_median_thresh2_dist())) * (m_median_max_dist() - difference_distances * m_dist_coeff()) + difference_intensities * m_intensity_coeff();
            }
         }
         certainty_value = std::min(certainty_value, 1.0f);
         certainty_value = std::max(certainty_value, 0.0f);

          if(certainty_value >= m_certainty_threshold())
          {
             DebugOutputPoint outputPoint;
             unsigned int index_of_current_point = clouds_per_ring[ring_index][point_index];
             outputPoint.x = cloud.points[index_of_current_point].x;
             outputPoint.y = cloud.points[index_of_current_point].y;
             outputPoint.z = cloud.points[index_of_current_point].z;
             outputPoint.ring = cloud.points[index_of_current_point].ring;
             outputPoint.intensity = cloud.points[index_of_current_point].intensity;
             outputPoint.detection_distance = difference_distances;
             outputPoint.detection_intensity = difference_intensities;

             outputPoint.detection = certainty_value;

             obstacle_cloud.push_back(outputPoint);
          }
      }
   }
   m_pub_obstacle_cloud.publish(obstacle_cloud);
}

void VelodyneObjectDetector::velodyneCallback(const InputPointCloud& input_cloud)
{
   ROS_DEBUG_STREAM("callback with thresh " << m_certainty_threshold());
   pcl::StopWatch timer;
   double start = timer.getTime();

   InputPointCloud cloud = input_cloud;

   // save indices of points in one ring in one vector
   // and each vector representing a ring in another vector containing all indices of the cloud
   std::vector<std::vector<unsigned int> > clouds_per_ring(PUCK_NUM_RINGS, std::vector<unsigned int>(0));
   splitCloudByRing(cloud, clouds_per_ring);

   // save first and last index of a segment within a ring in a pair
   // all segments of a ring in a vector
   // and all rings in another vector
   detectSegmentsMedian(cloud, clouds_per_ring);
   ROS_DEBUG_STREAM("Computation time for obstacle detection in ms " << (timer.getTime()- start) << "   \n");

   m_pub.publish(cloud);
}

}

int main(int argc, char **argv)
{
   ros::init(argc, argv, "velodyne_object_detector");

   velodyne_object_detector::VelodyneObjectDetector object_detector;

   ros::spin();

   return 0;
}
