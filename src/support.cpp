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

/**
  * Returns obstacle indexes of {
  *   lane 0 ahead, lane 1 ahead, lane 2 ahead,
  *   lane 0 behind, lane 1 behind, lane 2 behind
  * }
  *
  * For each of the above, if not found, use -1.
  */
vector<int> find_closest_obstacles(const vector<vector<double> > & obstacles, double ref_s) {

  vector<int> obstacle_inds(NUM_LANES * 2, -1);

  vector<double> closest_s_ahead(NUM_LANES, MAX_DOUBLE);
  vector<double> closest_s_behind(NUM_LANES, -MAX_DOUBLE);

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

Obstacle obstacle_vector_to_struct(const vector <double> & vec) {
  Obstacle obstacle;
  obstacle.id = vec[0];
  obstacle.now_x = vec[1];
  obstacle.now_y = vec[2];
  obstacle.now_speed = sqrt(pow(vec[3], 2) + pow(vec[4], 2));
  obstacle.now_s = vec[5];
  obstacle.now_d = vec[6];
  return obstacle;
}

/**
  * Retrieves obstacle based on object indexes returned by `find_closest_obstacles`.
  *
  * Returns (
  *   whether found,
  *   the found obstacle or the cpp default Obstacle struct
  * )
  */
tuple<bool, Obstacle> retrieve_closest_obstacle(
  int lane, bool ahead,
  const vector<vector<double> > & obstacles,
  const vector<int> & closest_obstacle_inds) {

  Obstacle obstacle;

  int obstacle_ind = closest_obstacle_inds[lane + (ahead ? 0 : NUM_LANES)];
  if (obstacle_ind == -1) {
    return std::make_tuple(false, obstacle);
  }

  obstacle = obstacle_vector_to_struct(obstacles[obstacle_ind]);
  return std::make_tuple(true, obstacle);
}

ObstacleRelationship calc_obstacle_relationship(
  const Telemetry & telemetry, const Obstacle & obstacle, bool eval_in_future) {

  double self_pos = eval_in_future ? telemetry.future_s : telemetry.now_s;
  double self_speed = eval_in_future ? telemetry.future_speed : telemetry.now_speed;

  double obst_pos = eval_in_future ?
    obstacle.now_s + obstacle.now_speed * telemetry.future_path_duration: // assume constant speed
    obstacle.now_s;

  // This math works whether looking ahead or behind.
  double pos_gap = obst_pos - self_pos;
  double speed_gap = obstacle.now_speed - self_speed;
  double time_to_collision = -pos_gap / speed_gap;

  double abs_pos_gap = fabs(pos_gap);
  bool is_pos_gap_safe = abs_pos_gap > LANE_CHANGE_BUFFER_S;

  bool is_time_to_collision_safe =
    time_to_collision < MIN_TIME_TO_COLLISION ||
    time_to_collision > MAX_TIME_TO_COLLISION;

  return ObstacleRelationship {
    abs_pos_gap, is_pos_gap_safe, time_to_collision, is_time_to_collision_safe};
}

/**
  * Returns whether setting the given lane as the target is .... TODO
  * 1) safe to do, and
  * 2) advantageous compared to reference time-to-collision.
  *
  * future only ... TODO
  *
  * Returns (whether safe, constraints) for the given lane.
  */
tuple<bool, LaneConstraints> estimate_lane_change(
  const int lane,
  const vector<int> & closest_obstacle_inds,
  const Telemetry & telemetry,
  const bool debug) {

  bool found_ahead, found_behind;
  Obstacle obstacle_ahead, obstacle_behind;
  std::tie(found_ahead, obstacle_ahead) =
    retrieve_closest_obstacle(lane, true, telemetry.now_obstacles, closest_obstacle_inds);
  std::tie(found_behind, obstacle_behind) =
    retrieve_closest_obstacle(lane, false, telemetry.now_obstacles, closest_obstacle_inds);

  if (! found_ahead && ! found_behind) {
    // No one ahead or behind.
    return std::make_tuple(true, LaneConstraints {SPEED_LIMIT, MAX_DOUBLE});
  }

  // Determine whether the obstacle behind will allow the self to be in front of it.
  if (found_behind) {
    ObstacleRelationship rel_behind = calc_obstacle_relationship(telemetry, obstacle_behind, true);

    if (debug) {
      cout << "estimate change to lane " << lane << " behind" << endl << rel_behind;
    }

    bool behind_is_safe = rel_behind.is_position_gap_safe && rel_behind.is_time_to_collision_safe;

    if (! behind_is_safe) {
      LaneConstraints foo;
      return std::make_tuple(false, foo);
    }
  }

  // At this point in code, we don't have to worry about obstacle behind
  // (whether there is one behind or not).

  if (! found_ahead) {
    // No one ahead.
    return std::make_tuple(true, LaneConstraints {SPEED_LIMIT, MAX_DOUBLE});
  }

  // At this point in code, there is someone ahead.

  ObstacleRelationship rel_ahead = calc_obstacle_relationship(telemetry, obstacle_ahead, true);

  if (debug) {
    cout << "estimate change to lane " << lane << " ahead" << endl << rel_ahead;
  }

  bool ahead_is_safe = rel_ahead.is_position_gap_safe && rel_ahead.is_time_to_collision_safe;

  if (ahead_is_safe) {
    return std::make_tuple(true, LaneConstraints {obstacle_ahead.now_speed, rel_ahead.time_to_collision});
  }

  LaneConstraints foo;
  return std::make_tuple(false, foo);
}

/**
  * Returns (target lane, constraints) for the most optimal adjacent lane,
  * where the target lane is -1 if there is no optimal adjacent lane.
  */
tuple<int, LaneConstraints> prepare_lane_change(
  const int current_lane,
  const vector<int> & closest_obstacle_inds,
  const Telemetry & telemetry,
  const bool debug) {

  vector<int> adjacent_lanes;
  if (current_lane == 0 || current_lane == 2) {
    adjacent_lanes = {1};
  } else {
    adjacent_lanes = {0, 2};
  }

  int optimal_adj_lane = -1;
  LaneConstraints optimal_adj_constraints {-MAX_DOUBLE, -MAX_DOUBLE};

  for (unsigned int temp = 0; temp < adjacent_lanes.size(); temp++) {
    int adj_lane = adjacent_lanes[temp];

    bool is_safe;
    LaneConstraints constraints;
    std::tie(is_safe, constraints) = estimate_lane_change(
      adj_lane, closest_obstacle_inds, telemetry, debug);

    if (is_safe &&
        constraints.time_to_collision > optimal_adj_constraints.time_to_collision) {
      optimal_adj_lane = adj_lane;
      optimal_adj_constraints = constraints;
    }
  }

  cout << "optimal lane to change into is " << optimal_adj_lane << endl << optimal_adj_constraints;

  return std::make_tuple(optimal_adj_lane, optimal_adj_constraints);
}

/**
  * Evaluates the self's relationship with the object ahead in the target lane,
  * at the present time and, if necessary, in the future.
  * Evaluating the present-time relationship is necessary because an obstacle
  * may cut into self's lane at a closer proximity than the self's future path end.
  */
LaneConstraints follow_obstacle_ahead(
  const int target_lane,
  const vector<int> & closest_obstacle_inds,
  const Telemetry & telemetry,
  const bool debug) {

  bool obstacle_was_found;
  Obstacle obstacle;
  std::tie(obstacle_was_found, obstacle) = retrieve_closest_obstacle(
    target_lane, true, telemetry.now_obstacles, closest_obstacle_inds);

  if (! obstacle_was_found) {
    return LaneConstraints {SPEED_LIMIT, MAX_DOUBLE};
  }

  ObstacleRelationship now_rel = calc_obstacle_relationship(telemetry, obstacle, false);

  if (debug) {
    cout << "following, now:" << endl << now_rel;
  }

  // Skip checking position safety.
  if (! now_rel.is_time_to_collision_safe) {
    return LaneConstraints {obstacle.now_speed, now_rel.time_to_collision};
  }

  ObstacleRelationship future_rel = calc_obstacle_relationship(telemetry, obstacle, true);

  if (debug) {
    cout << "following, future:" << endl << future_rel;
  }

  // Skip checking position safety.
  if (! future_rel.is_time_to_collision_safe) {
    return LaneConstraints {obstacle.now_speed, future_rel.time_to_collision};
  }

  return LaneConstraints {SPEED_LIMIT, MAX_DOUBLE};
}

FSM iterate_fsm(const FSM fsm, const Telemetry & telemetry, const bool debug) {

  // Use the self's current position as the reference when finding closest obstacles.
  vector<int> closest_obstacle_inds = find_closest_obstacles(telemetry.now_obstacles, telemetry.now_s);

  // For all FSM states,
  // 1. adjust the target speed for the current target lane
  // 1. also get the cost of keeping the current target lane
  //
  // Time-to-collision is the cost function.
  // The less positive the time-to-collision, the worse the cost.
  LaneConstraints following = follow_obstacle_ahead(
    fsm.target_lane,
    closest_obstacle_inds,
    telemetry,
    debug);

  if (fsm.state == KEEP_LANE) {
    bool ahead_is_clear = following.time_to_collision == MAX_DOUBLE;
    FSM_State next_fsm_state = ahead_is_clear ? KEEP_LANE : PLAN_LANE_CHANGE;
    return FSM {next_fsm_state, fsm.target_lane, following.target_speed};
  }

  if (fsm.state == PLAN_LANE_CHANGE) {
    int adj_lane;
    LaneConstraints adj_constraints;
    std::tie(adj_lane, adj_constraints) = prepare_lane_change(
      fsm.target_lane, closest_obstacle_inds, telemetry, debug);

    if (adj_lane != -1 &&
        adj_constraints.time_to_collision > following.time_to_collision) {
      return FSM {CHANGING_LANE, adj_lane, adj_constraints.target_speed};
    } else {
      return FSM {PLAN_LANE_CHANGE, fsm.target_lane, following.target_speed};
    }
  }

  if (fsm.state == CHANGING_LANE) {
    double d_error = fabs(lane_index_to_d(fsm.target_lane) - telemetry.future_d);
    if (d_error < LANE_CHANGE_COMPLETION_MARGIN) {
      // Changing lane is done.
      return FSM {KEEP_LANE, fsm.target_lane, following.target_speed};
    }


    // TODO
    return FSM {CHANGING_LANE, fsm.target_lane, following.target_speed};
    // TODO restore below


    // // We check for the feasibility of the destination lane as we're moving into it.
    // // This is necessary because there might be an obstacle behind that's closer
    // // than expected. I believe this happens when that obstacle either:
    // // - was in the same lane and accelerated
    // // - came in from another lane
    // double _, alt_time_to_collision;
    // std::tie(_, alt_time_to_collision) = estimate_lane_change(
    //   fsm.target_lane,
    //   closest_obstacle_inds,
    //   telemetry,
    //   debug);

    // if (alt_time_to_collision < MIN_TIME_TO_COLLISION) {
    //   if (debug) {
    //     cout << "abort changing lane !!!" << endl;
    //   }
    //   // Pass the current `telemetry.now_d`, rather than the future `end_path_d`.
    //   int original_target_lane = lane_index_away(fsm.target_lane, telemetry.now_d);
    //   return FSM {CHANGING_LANE, original_target_lane, following.target_speed};
    // } else {
    //   return FSM {CHANGING_LANE, fsm.target_lane, following.target_speed};
    // }
  }

  throw; // Remove compiler warning
}

tuple<vector<double>, vector<double> > generate_path(
  const int target_lane, const double end_path_speed, const Telemetry & telemetry,
  const std::function<vector<double>(double, double)> & sd_to_xy) {

  // Markers for spline. Spline will go through all points defined here.
  vector<double> markers_x, markers_y;

  double end_path_x, end_path_y, end_path_yaw;

  // add 2 previous points to markers
  if (telemetry.future_path_size < 2) {
    end_path_x = telemetry.now_x;
    end_path_y = telemetry.now_y;
    end_path_yaw = telemetry.now_yaw;

    markers_x.push_back(telemetry.now_x - cos(telemetry.now_yaw));
    markers_y.push_back(telemetry.now_y - sin(telemetry.now_yaw));

    markers_x.push_back(end_path_x);
    markers_y.push_back(end_path_y);
  } else {
    end_path_x = telemetry.future_path_x[telemetry.future_path_size - 1];
    end_path_y = telemetry.future_path_y[telemetry.future_path_size - 1];
    double penultimate_x = telemetry.future_path_x[telemetry.future_path_size - 2];
    double penultimate_y = telemetry.future_path_y[telemetry.future_path_size - 2];
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
      telemetry.future_s + future_s_offset,
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
  vector<double> next_path_x(telemetry.future_path_size), next_path_y(telemetry.future_path_size);

  // Add back all previous path points
  std::copy(telemetry.future_path_x.begin(), telemetry.future_path_x.end(), next_path_x.begin());
  std::copy(telemetry.future_path_y.begin(), telemetry.future_path_y.end(), next_path_y.begin());

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
  for (int i = 0; i < NUM_OUTPUT_PATH_POINTS - telemetry.future_path_size; i++) {
    
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
