#include "support.h"
#include "spline.h"
#include <iostream>

using std::cout;
using std::endl;
using std::tuple;
using std::vector;

double lane_index_to_d(int lane_index) {
  return LANE_WIDTH / 2 + LANE_WIDTH * lane_index;
}

int lane_index_away(int from_lane_index, double to_d) {
  double from_d = lane_index_to_d(from_lane_index);
  if (from_d == to_d) {
    return from_lane_index;
  } else if (from_d < to_d) {
    return std::min(NUM_LANES - 1, from_lane_index + 1);
  } else {
    return std::max(0, from_lane_index - 1);
  }
}

vector<int> find_closest_obstacles(const vector<vector<double> > & obstacles, double ref_s) {

  vector<int> obstacle_inds(NUM_LANES * 2, -1);

  vector<double> closest_s_ahead(NUM_LANES, std::numeric_limits<double>::max());
  vector<double> closest_s_behind(NUM_LANES, - std::numeric_limits<double>::max());

  for (int obstacle_ind = 0; obstacle_ind < obstacles.size(); obstacle_ind++) {
    vector<double> obstacle = obstacles[obstacle_ind];
    double d = obstacle[6];
    double s = obstacle[5];

    vector<int> relevant_lanes;
    if (d < LANE_WIDTH + LANE_DETECTION_BUFFER) {
      relevant_lanes.push_back(0);
    }
    if (LANE_WIDTH - LANE_DETECTION_BUFFER < d && d < LANE_WIDTH * 2 + LANE_DETECTION_BUFFER) {
      relevant_lanes.push_back(1);
    }
    if (LANE_WIDTH * 2 - LANE_DETECTION_BUFFER < d) {
      relevant_lanes.push_back(2);
    }

    for(unsigned int i = 0; i < relevant_lanes.size(); i++) {
      int lane = relevant_lanes[i];

      if (s > ref_s && s < closest_s_ahead[lane]) {
        closest_s_ahead[lane] = s;
        obstacle_inds[lane] = obstacle_ind;
      } else if (s < ref_s && s > closest_s_behind[lane]) {
        closest_s_behind[lane] = s;
        obstacle_inds[lane + NUM_LANES] = obstacle_ind;
      }
    }
  }

  return obstacle_inds;
}

double extract_obstacle_speed(const vector<double> & obstacle) {
  double vx = obstacle[3];
  double vy = obstacle[4];
  return sqrt(pow(vx, 2) + pow(vy, 2));
}

double extract_obstacle_position(const vector<double> & obstacle) {
  return obstacle[5];
}

tuple<double, double> is_lane_feasible(
    const int lane,
    const vector<vector<double> > & obstacles, const vector<int> & closest_obstacle_inds,
    const double end_path_s, const double end_path_speed,
    const double prev_path_duration,
    const bool debug) {

  int obstacle_ind_ahead = closest_obstacle_inds[lane];
  int obstacle_ind_behind = closest_obstacle_inds[lane + NUM_LANES];

  if (obstacle_ind_ahead == -1 && obstacle_ind_behind == -1) {
    // No one ahead or behind.
    return std::make_tuple(SPEED_LIMIT, std::numeric_limits<double>::max());
  }

  // Determine whether the obstacle behind allows the self to be in front of it.
  if (obstacle_ind_behind != -1) {
    vector<double> obstacle_behind = obstacles[obstacle_ind_behind];
    double obst_speed = extract_obstacle_speed(obstacle_behind);
    double obst_pos_endpath = extract_obstacle_position(obstacle_behind) + obst_speed * prev_path_duration;

    double dist_gap = end_path_s - obst_pos_endpath;
    double speed_gap = end_path_speed - obst_speed;
    double time_to_collision = -dist_gap / speed_gap;

    bool behind_is_ok =
      dist_gap > LANE_CHANGE_BUFFER_S &&
      (time_to_collision < MIN_TIME_TO_COLLISION ||
        time_to_collision > MAX_TIME_TO_COLLISION);

    if (debug) {
      cout << "v dist_gap "
        << (dist_gap > LANE_CHANGE_BUFFER_S ? "" : "! ")
        << dist_gap << endl;
      cout << "v time_to_collision "
        << (time_to_collision < MIN_TIME_TO_COLLISION ||
            time_to_collision > MAX_TIME_TO_COLLISION ? "" : "! ")
        << time_to_collision << endl;
    }

    if (! behind_is_ok) {
      return std::make_tuple(0, -std::numeric_limits<double>::max());
    }
  }
  // At this point, the obstacle behind does allow the self to be in front of it.

  if (obstacle_ind_ahead == -1) {
    // No one ahead.
    return std::make_tuple(SPEED_LIMIT, std::numeric_limits<double>::max());
  }

  // There is someone ahead.
  vector<double> obstacle_ahead = obstacles[obstacle_ind_ahead];
  double obst_speed = extract_obstacle_speed(obstacle_ahead);
  double obst_pos_endpath = extract_obstacle_position(obstacle_ahead) + obst_speed * prev_path_duration;

  double dist_gap = obst_pos_endpath - end_path_s;
  double speed_gap = obst_speed - end_path_speed;
  double time_to_collision = -dist_gap / speed_gap;

  bool ahead_is_ok = 
    dist_gap > LANE_CHANGE_BUFFER_S &&
    time_to_collision > MAX_TIME_TO_COLLISION;

  if (debug) {
    cout << "^ dist_gap "
      << (dist_gap > LANE_CHANGE_BUFFER_S ? "" : "! ")
      << dist_gap << endl;
    cout << "^ time_to_collision "
      << (time_to_collision < MIN_TIME_TO_COLLISION ||
          time_to_collision > MAX_TIME_TO_COLLISION ? "" : "! ")
      << time_to_collision << endl;
  }

  if (ahead_is_ok) {
    return std::make_tuple(obst_speed, time_to_collision);
  }

  return std::make_tuple(0, -std::numeric_limits<double>::max());
}

tuple<int, double, double> prepare_lane_change(
    const int target_lane, const double target_speed,
    const vector<vector<double> > & obstacles, const vector<int> & closest_obstacle_inds,
    const double end_path_s, const double end_path_speed,
    const double prev_path_duration,
    const bool debug) {

  vector<int> adjacent_lanes; // TODO compare all time-to-collisions.
  if (target_lane == 0 || target_lane == 2) {
    adjacent_lanes = {1};
  } else {
    adjacent_lanes = {0, 2};
  }

  int optimal_target_lane = -1;
  double optimal_target_speed;
  double optimal_time_to_collision = -std::numeric_limits<double>::max();

  for (unsigned int temp = 0; temp < adjacent_lanes.size(); temp++) {
    int adj_lane = adjacent_lanes[temp];
    if (debug) {
      cout << "adj_lane " << adj_lane << endl;
    }
    
    double adj_lane_target_speed, adj_lane_time_to_collision;
    std::tie(adj_lane_target_speed, adj_lane_time_to_collision) = is_lane_feasible(
      adj_lane,
      obstacles, closest_obstacle_inds,
      end_path_s, end_path_speed,
      prev_path_duration,
      debug);

    if (adj_lane_time_to_collision > optimal_time_to_collision) {
      optimal_target_lane = adj_lane;
      optimal_target_speed = adj_lane_target_speed;
      optimal_time_to_collision = adj_lane_time_to_collision;
    }
  }

  if (optimal_target_lane == -1) {
    return std::make_tuple(0, 0, -std::numeric_limits<double>::max());
  } else {
    return std::make_tuple(optimal_target_lane, optimal_target_speed, optimal_time_to_collision);
  }
}

tuple<double, double> keep_lane(
    const int target_lane,
    const vector<vector<double> > & obstacles, const vector<int> & closest_obstacle_inds,
    const double car_s, const double car_speed,
    const double end_path_s, const double end_path_speed,
    const double prev_path_duration,
    const bool debug) {

  int obstacle_ind_ahead = closest_obstacle_inds[target_lane];
  
  if (obstacle_ind_ahead == -1) {
    return std::make_tuple(SPEED_LIMIT, std::numeric_limits<double>::max());
  }

  vector<double> obstacle_ahead = obstacles[obstacle_ind_ahead];
  double obst_pos_now = extract_obstacle_position(obstacle_ahead);
  double obst_speed = extract_obstacle_speed(obstacle_ahead);

  double dist_gap_now = obst_pos_now - car_s;
  double speed_gap_now = obst_speed - car_speed;
  double time_to_collision_now = -dist_gap_now / speed_gap_now;

  // We need to consider obstacles that cut in front of us at a distance between
  // current `car_s` and future `end_path_s`.
  bool too_close_now =
    time_to_collision_now > MIN_TIME_TO_COLLISION &&
    time_to_collision_now < MAX_TIME_TO_COLLISION;

  if (debug) {
    cout << "^ time_to_collision_now "
      << (too_close_now ? "! " : "")
      << time_to_collision_now << endl;
    if (too_close_now) {
      // Keep the num of lines output by this fn constant.
      cout << "^ time_to_collision_endpath -----" << endl;
    }
  }

  if (too_close_now) {
    return std::make_tuple(obst_speed, time_to_collision_now);
  }

  double obst_pos_endpath = obst_pos_now + obst_speed * prev_path_duration;
  double dist_gap_endpath = obst_pos_endpath - end_path_s;
  double speed_gap_endpath = obst_speed - end_path_speed;
  double time_to_collision_endpath = -dist_gap_endpath / speed_gap_endpath;

  bool too_close_endpath =
    time_to_collision_endpath > MIN_TIME_TO_COLLISION &&
    time_to_collision_endpath < MAX_TIME_TO_COLLISION;

  if (debug) {
    cout << "^ time_to_collision_endpath "
      << (too_close_endpath ? "! " : "")
      << time_to_collision_endpath << endl;
  }

  if (too_close_endpath) {
    return std::make_tuple(obst_speed, time_to_collision_endpath);
  }

  // Obstacle ahead is still far.
  return std::make_tuple(SPEED_LIMIT, std::numeric_limits<double>::max());
}

tuple<FSM_State, int, double> iterate_fsm(
    const FSM_State fsm_state, const int target_lane, const double target_speed,
    const vector<vector<double> > & obstacles,
    const double car_s, const double car_d, const double car_speed,
    const double end_path_s, const double end_path_d, const double end_path_speed,
    const size_t previous_path_size,
    const bool debug) {

  double prev_path_duration = PATH_INTERVAL * previous_path_size;

  // Use the self's current position as the reference when finding closest obstacles.
  vector<int> closest_obstacle_inds = find_closest_obstacles(obstacles, car_s);

  // For all FSM states,
  // 1. adjust the target speed for the current target lane
  // 1. also get the cost of keeping the current target lane
  //
  // Time-to-collision is the cost function.
  // The less positive the time-to-collision, the worse the cost.
  double default_target_speed, default_time_to_collision;
  std::tie(default_target_speed, default_time_to_collision) = keep_lane(
    target_lane,
    obstacles, closest_obstacle_inds,
    car_s, car_speed,
    end_path_s, end_path_speed,
    prev_path_duration,
    debug);

  if (fsm_state == KEEP_LANE) {
    bool default_is_optimal =
      default_time_to_collision == std::numeric_limits<double>::max();
    FSM_State next_fsm_state = default_is_optimal ? KEEP_LANE : PLAN_LANE_CHANGE;
    return std::make_tuple(next_fsm_state, target_lane, default_target_speed);
  }

  if (fsm_state == PLAN_LANE_CHANGE) {
    int alt_target_lane;
    double alt_target_speed, alt_time_to_collision;
    std::tie(alt_target_lane, alt_target_speed, alt_time_to_collision) =
      prepare_lane_change(
        target_lane, target_speed,
        obstacles, closest_obstacle_inds,
        end_path_s, end_path_speed,
        prev_path_duration,
        debug);

    if (alt_time_to_collision < default_time_to_collision) {
      return std::make_tuple(CHANGING_LANE, alt_target_lane, alt_target_speed);
    } else {
      return std::make_tuple(PLAN_LANE_CHANGE, target_lane, default_target_speed);
    }
  }

  if (fsm_state == CHANGING_LANE) {
    double d_error = fabs(lane_index_to_d(target_lane) - end_path_d);
    if (d_error < LANE_CHANGE_COMPLETION_MARGIN) {
      // Changing lane is done.
      return std::make_tuple(KEEP_LANE, target_lane, default_target_speed);
    }

    // We check for the feasibility of the destination lane as we're moving into it.
    // This is necessary because there might be an obstacle behind that's closer
    // than expected. I believe this happens when that obstacle either:
    // - was in the same lane and accelerated
    // - came in from another lane
    double _, alt_time_to_collision;
    std::tie(_, alt_time_to_collision) = is_lane_feasible(
      target_lane,
      obstacles, closest_obstacle_inds,
      end_path_s, end_path_speed,
      prev_path_duration,
      debug);

    if (alt_time_to_collision < MIN_TIME_TO_COLLISION) {
      if (debug) {
        cout << "abort changing lane !!!" << endl;
      }
      // Pass the current `car_d`, rather than the future `end_path_d`.
      int original_target_lane = lane_index_away(target_lane, car_d);
      return std::make_tuple(CHANGING_LANE, original_target_lane, default_target_speed);
    } else {
      return std::make_tuple(CHANGING_LANE, target_lane, default_target_speed);
    }
  }

  throw; // Remove compiler warning
}

tuple<vector<double>, vector<double> > generate_path(
    const double car_x, const double car_y, const double car_yaw,
    const vector<double> & previous_path_x, const vector<double> & previous_path_y, const size_t previous_path_size,
    const double end_path_s, const double end_path_speed, const int target_lane,
    const std::function<vector<double>(double, double)> & sd_to_xy) {

  // Markers for spline. Spline will go through all points defined here.
  vector<double> markers_x, markers_y;

  double end_path_x, end_path_y, end_path_yaw;

  // add 2 previous points to markers
  if (previous_path_size < 2) {
    end_path_x = car_x;
    end_path_y = car_y;
    end_path_yaw = car_yaw;

    markers_x.push_back(car_x - cos(car_yaw));
    markers_y.push_back(car_y - sin(car_yaw));

    markers_x.push_back(end_path_x);
    markers_y.push_back(end_path_y);
  } else {
    end_path_x = previous_path_x[previous_path_size - 1];
    end_path_y = previous_path_y[previous_path_size - 1];
    double penultimate_x = previous_path_x[previous_path_size - 2];
    double penultimate_y = previous_path_y[previous_path_size - 2];
    end_path_yaw = atan2(end_path_y - penultimate_y, end_path_x - penultimate_x);

    markers_x.push_back(penultimate_x);
    markers_y.push_back(penultimate_y);

    markers_x.push_back(end_path_x);
    markers_y.push_back(end_path_y);
  }

  // Add a few more sparse markers in far distance
  // Pick the markers in s,d and convert them to x,y
  // 30 meters is far ahead enough for a smooth lane change
  // Add more points further away to make the tail end of the curve straight
  vector<double> future_s_offsets{30, 60, 90};
  for (unsigned int i = 0; i < future_s_offsets.size(); i++) {
    double future_s_offset = future_s_offsets[i];

    vector<double> marker_xy = sd_to_xy(
      end_path_s + future_s_offset,
      lane_index_to_d(target_lane));

    markers_x.push_back(marker_xy[0]);
    markers_y.push_back(marker_xy[1]);
  }

  // Transform markers to coordinate system - which I'll call p,q - such that
  // origin is at previous path's end
  // and p is pointing to car's forward direction at previous path's end
  // Reuse the same vectors
  for (int i = 0; i < markers_x.size(); i++) {
    double translated_x = markers_x[i] - end_path_x;
    double translated_y = markers_y[i] - end_path_y;
    markers_x[i] = translated_x * cos(-end_path_yaw) - translated_y * sin(-end_path_yaw);
    markers_y[i] = translated_x * sin(-end_path_yaw) + translated_y * cos(-end_path_yaw);
  }

  // Spline in p,q
  tk::spline spliner_pq;
  spliner_pq.set_points(markers_x, markers_y);

  // Next path x,y. This is what we will output.
  // The vector length will be greater than `previous_path_size`.
  vector<double> next_path_x(previous_path_size), next_path_y(previous_path_size);

  // Add back all previous path points
  std::copy(previous_path_x.begin(), previous_path_x.end(), next_path_x.begin());
  std::copy(previous_path_y.begin(), previous_path_y.end(), next_path_y.begin());

  // Add newly generated path points.
  // First determine at what interval.
  double p_interval;
  {
    // Pick an arbitrary p in the future. It should be both far enough
    // and close enough to linearize the curvature.
    // Because s and p coordinates are approximately equal in the short
    // distance, simply pick the closest future s offset.
    double target_p = future_s_offsets[0];
    double target_q = spliner_pq(target_p);
    double target_dist = sqrt(pow(target_p, 2) + pow(target_q, 2));
    double dist_per_interval = PATH_INTERVAL * end_path_speed / MPS_TO_MPH; // meter
    double N = target_dist / dist_per_interval;
    p_interval = target_p / N;
  }

  // Add as many points as needed to refill the count of output path points.
  // Generate points in p,q; then transform to x,y
  for (int i = 0; i < NUM_OUTPUT_PATH_POINTS - previous_path_size; i++) {
    
    // To avoid repeating end_path_x and end_path_y, must use `i+1`. Makes a big difference!
    double p = p_interval * (i + 1);
    double q = spliner_pq(p);

    double x = p * cos(end_path_yaw) - q * sin(end_path_yaw) + end_path_x;
    double y = p * sin(end_path_yaw) + q * cos(end_path_yaw) + end_path_y;

    next_path_x.push_back(x);
    next_path_y.push_back(y);
  }

  return std::make_tuple(next_path_x, next_path_y);
}
