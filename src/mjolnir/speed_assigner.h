#pragma once

#include "baldr/rapidjson_utils.h"
#include "filesystem.h"

// Factors used to adjust speed assignments
constexpr float kTurnChannelFactor = 1.25f;
constexpr float kRampDensityFactor = 0.8f;
constexpr float kRampFactor = 0.85f;
constexpr float kRoundaboutFactor = 0.5f;

/*
The json basically looks like this:

[{
  "iso3166-1": "us",
  "iso3166-2": "pa",
  "urban": {
    "way": [1,2,3,4,5,6,7,8],
    "link_exiting": [9,10,11,12,13],
    "link_turning": [15,16,17,18,19],
    "roundabout": [21,22,23,24,25,26,27,28],
    "driveway": 29,
    "alley": 30,
    "parking_aisle": 31,
    "drive-through": 32
  },
  "rural": {
    "way": [33,34,35,36,37,38,39,40],
    "link_exiting": [41,42,43,44,45],
    "link_turning": [47,48,49,50,51],
    "roundabout": [53,54,55,56,57,58,59,60],
    "driveway": 61,
    "alley": 62,
    "parking_aisle": 63,
    "drive-through": 64
  }
}]
 */

/**
 * This class has two methods of deciding the default speed of an edge. The default method is to use
 * some of the constants above and some of the attributes on the edge to modify the existing speed.
 * This method can be overridden by a json config which allows for geography specific speed assignment
 * by country/state, urban/rural, road class, road use and link/ramp type
 */
class SpeedAssigner {
protected:
  struct SpeedTable {
    // no special uses
    std::array<uint32_t, 8> way;
    // ramps
    std::array<uint32_t, 5> link_exiting;
    // turn channel
    std::array<uint32_t, 5> link_turning;
    // roundabout
    std::array<uint32_t, 8> roundabout;
    // driveway, alley, parking_aisle, drive-through
    std::array<uint32_t, 4> service;

    template <typename container_t>
    void parse(const rapidjson::Value& obj, const std::string& name, container_t& entries) {
      auto arr = obj[name].GetArray();
      if (arr.Size() != entries.size())
        throw std::runtime_error(name + " must have " + std::to_string(entries.size()) + " speeds");
      size_t i = 0;
      for (const auto& speed : arr) {
        entries[i++] = speed.GetUint();
      }
    }

    SpeedTable(const rapidjson::Value& obj) {
      parse(obj, "way", way);
      parse(obj, "link_exiting", link_exiting);
      parse(obj, "link_turning", link_turning);
      parse(obj, "roundabout", roundabout);
      service[0] = obj["driveway"].GetUint();
      service[1] = obj["alley"].GetUint();
      service[2] = obj["parking_aisle"].GetUint();
      service[3] = obj["drive-through"].GetUint();
    }
  };

  // 2 letter country and 2 letter state as key with urban and rural speed tables as value
  std::unordered_map<std::string, std::array<SpeedTable, 2>> tables;

  SpeedAssigner(const boost::optional<std::string>& config_file) {
    if (!config_file) {
      LOG_INFO("Disabled default speeds assignment from config");
      return;
    }
    try {
      auto doc = rapidjson::read_json(*config_file);
      if (doc.HasParseError())
        throw std::runtime_error("malformed json");
      if (!doc.IsArray())
        throw std::runtime_error("must be a json array");

      // loop over each country/state pair
      for (const auto& cs : doc.GetArray()) {
        std::string code = cs["iso3166-1"].GetString();
        code += ".";
        code += cs["iso3166-2"].GetString();
        tables.emplace(std::move(code), std::array<SpeedTable, 2>{
            SpeedTable(cs["urban"]),
            SpeedTable(cs["rural"]),
        });
      }
    } // something went wrong with parsing or opening the file
    catch (const std::exception& e) {
      LOG_WARN(std::string("Disabled default speeds assignment from config: ") + e.what());
      tables.clear();
    } // something else was thrown
    catch (...) {
      LOG_WARN("Disabled default speeds assignment from config: unknown error");
      tables.clear();
    }
    LOG_INFO("Enabled default speeds assignment from config: " + *config_file);
  }

  /**
   * This function determines the speed of an edge based on the json configuration provided to the
   * classes constructor. If the edge is one of the types that cannot be assigned via config the
   * method will leave the edges speed unset and signal as much by returning false
   *
   * @param directededge         the edge whose speed we may set
   * @param density              the road density of the end node of the edge
   * @param country_state_code   the 4 letter country/state code, it may be 2 letters if just the
   *                             country is known and it may also be an empty string if no admin
   *                             information has been loaded
   * @return true if the directededge had its speed set from configuration
   */
  bool FromConfig(DirectedEdge& directededge,
                  const uint32_t density,
                  const std::string& country,
                  const std::string& state) const {
    // let the other function handle ferry stuff or anything not motor vehicle
    if (directededge.use() == Use::kFerry || directededge.use() == Use::kRailFerry ||
        !((directededge.forwardaccess() | directededge.reverseaccess()) & kVehicularAccess))
      return false;

    // try first the country state combo, then country only, then neither, then bail
    auto found = tables.find(country + "." + state);
    if (found == tables.end())
      found = tables.find(country + ".");
    if (found == tables.end())
      found = tables.find("");
    if (found == tables.end())
      return false;

    // urban or rural
    const auto& speed_table = found->second[density <= 8];
    size_t rc = static_cast<size_t>(directededge.classification());

    // some kind of special use
    switch (directededge.use()) {
      case Use::kDriveway:
        directededge.set_speed(speed_table.service[0]);
        return true;
      case Use::kAlley:
        directededge.set_speed(speed_table.service[1]);
        return true;
      case Use::kParkingAisle:
        directededge.set_speed(speed_table.service[2]);
        return true;
      case Use::kDriveThru:
        directededge.set_speed(speed_table.service[3]);
        return true;
      default:
        break;
    }

    // exit ramp
    if (directededge.link()) {
      // these classes dont have links
      if (rc >= speed_table.link_exiting.size())
        return false;
      // we use signage to tell if its an exit otherwise its just a link/ramp/turn channel
      if (directededge.sign())
        directededge.set_speed(speed_table.link_exiting[rc]);
      else
        directededge.set_speed(speed_table.link_turning[rc]);
      return true;
    }

    // roundabout
    if (directededge.roundabout()) {
      directededge.set_speed(speed_table.roundabout[rc]);
      return true;
    }

    // non-special use, just use the road class
    directededge.set_speed(speed_table.way[rc]);
    return true;
  }

public:
  static const SpeedAssigner& GetInstance(const boost::optional<std::string>& config_file) {
    static SpeedAssigner assigner(config_file);
    return assigner;
  }

  /**
   * Update directed edge speed based on density and other edge parameters like
   * surface type. TODO - add admin specific logic
   * @param  directededge  Directed edge to update.
   * @param  density       Relative road density.
   * @param  urban_rc_speed Array of default speeds vs. road class for urban areas
   * @param  rc_speed Array of default speeds vs. road class
   * @param  infer_turn_channels flag indicating if we should infer tc.
   * @param country_state_code 2 letter country and 2 letter state codes concatinated
   *
   */
  void UpdateSpeed(DirectedEdge& directededge,
                   const uint32_t density,
                   const uint32_t* urban_rc_speed,
                   bool infer_turn_channels,
                   const std::string& country_code,
                   const std::string& state_code) const {

    // If we have config loaded we'll use that
    if (!tables.empty() && FromConfig(directededge, density, country_code, state_code))
      return;

    // Update speed on ramps (if not a tagged speed) and turn channels
    if (directededge.link()) {
      uint32_t speed = directededge.speed();
      Use use = directededge.use();
      if (use == Use::kTurnChannel && infer_turn_channels) {
        speed = static_cast<uint32_t>((speed * kTurnChannelFactor) + 0.5f);
      } else if ((use == Use::kRamp) && (directededge.speed_type() != SpeedType::kTagged)) {
        // If no tagged speed set ramp speed to slightly lower than speed
        // for roads of this classification
        RoadClass rc = directededge.classification();
        if ((rc == RoadClass::kMotorway) || (rc == RoadClass::kTrunk) ||
            (rc == RoadClass::kPrimary)) {
          speed = (density > 8) ? static_cast<uint32_t>((speed * kRampDensityFactor) + 0.5f)
                                : static_cast<uint32_t>((speed * kRampFactor) + 0.5f);
        } else {
          speed = static_cast<uint32_t>((speed * kRampFactor) + 0.5f);
        }
      }
      directededge.set_speed(speed);

      // Done processing links so return...
      return;
    }

    // If speed is assigned from an OSM max_speed tag we only update it based
    // on surface type.
    if (directededge.speed_type() == SpeedType::kTagged) {
      // Reduce speed on rough pavements. TODO - do we want to increase
      // more on worse surface types?
      if (directededge.surface() >= Surface::kPavedRough) {
        uint32_t speed = directededge.speed();
        if (speed >= 50) {
          directededge.set_speed(speed - 10);
        } else if (speed > 15) {
          directededge.set_speed(speed - 5);
        }
      }
      return;
    }

    // Set speed on ferries. Base the speed on the length - assumes
    // that longer lengths generally use a faster ferry boat
    if (directededge.use() == Use::kRailFerry) {
      directededge.set_speed(65); // 40 MPH
      return;
    } else if (directededge.use() == Use::kFerry) {
      // if duration flag is set do nothing with speed - currently set
      // as the leaves tile flag.
      // leaves tile flag is updated later to the real value.
      if (directededge.leaves_tile()) {
        return;
      } else if (directededge.length() < 2000) {
        directededge.set_speed(10); // 5 knots
      } else if (directededge.length() < 8000) {
        directededge.set_speed(20); // 10 knots
      } else {
        directededge.set_speed(30); // 15 knots
      }
      return;
    }

    // Modify speed for roads in urban regions - anything above 8 is
    // assumed to be urban
    // if this density check is changed to be greater than 8, then we need to modify the urban flag in
    // the osrm response as well.
    if (density > 8) {
      uint32_t rc = static_cast<uint32_t>(directededge.classification());
      directededge.set_speed(urban_rc_speed[rc]);
    }

    if (directededge.roundabout()) {
      uint32_t speed = directededge.speed(); // could be default or urban speed
      directededge.set_speed(static_cast<uint32_t>((speed * kRoundaboutFactor) + 0.5f));
    }

    // Reduce speeds on parking aisles, driveways, and drive-thrus. These uses are
    // marked as destination only in pbfgraphparser.
    if (directededge.use() == Use::kParkingAisle) {
      directededge.set_speed(kParkingAisleSpeed);
    } else if (directededge.use() == Use::kDriveway) {
      directededge.set_speed(kDrivewaySpeed);
    } else if (directededge.use() == Use::kDriveThru) {
      directededge.set_speed(kDriveThruSpeed);
    }

    // Modify speed based on surface.
    if (directededge.surface() >= Surface::kPavedRough) {
      uint32_t speed = directededge.speed();
      directededge.set_speed(speed / 2);
    }
  }
};
