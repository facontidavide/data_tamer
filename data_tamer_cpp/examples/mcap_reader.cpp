#include "data_tamer_parser/data_tamer_parser.hpp"
#include <mcap/reader.hpp>

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Reads an MCAP file written by DataTamer (for instance test_sample.mcap from
// T03_mcap_writer) and counts the samples of each time series.
int main(int argc, char** argv)
{
  if(argc != 2)
  {
    std::cout << "add the MCAP file as argument" << std::endl;
    return 1;
  }
  std::string filepath = argv[1];

  // open the file
  mcap::McapReader reader;
  {
    auto const res = reader.open(filepath);
    if(!res.ok())
    {
      throw std::runtime_error("Can't open MCAP file");
    }
  }

  // start reading all the schemas and parsing them
  std::unordered_map<mcap::SchemaId, size_t> schema_id_to_hash;
  std::unordered_map<size_t, DataTamerParser::Schema> hash_to_schema;
  // must call this, before accessing the schemas
  auto summary = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  for(const auto& [schema_id, mcap_schema] : reader.schemas())
  {
    std::string schema_text(reinterpret_cast<const char*>(mcap_schema->data.data()),
                            mcap_schema->data.size());

    auto dt_schema = DataTamerParser::BuildSchemaFromText(schema_text);
    schema_id_to_hash[mcap_schema->id] = dt_schema.hash;
    hash_to_schema[dt_schema.hash] = dt_schema;
  }

  // count the samples of each time series; the values themselves are not used
  using MessageCount = std::map<std::string, size_t>;
  std::map<std::string, MessageCount> message_counts_per_channel;

  // parse all messages
  for(const auto& msg : reader.readMessages())
  {
    const std::string& channel_name = msg.channel->topic;
    const size_t schema_hash = schema_id_to_hash.at(msg.schema->id);
    const auto& dt_schema = hash_to_schema.at(schema_hash);

    // the callback invoked by ParseSnapshot for each value of the message
    std::vector<std::string> series;
    auto callback_number = [&](const std::string& series_name,
                               const DataTamerParser::VarNumber&) {
      series.push_back(series_name);
    };

    std::string problem;  // why the message is corrupt; empty if it decoded
    try
    {
      // the message body holds the active mask and the payload, one after the other
      auto snapshot = DataTamerParser::SplitMcapMessage(
          { reinterpret_cast<const uint8_t*>(msg.message.data), msg.message.dataSize });
      snapshot.timestamp = msg.message.logTime;
      snapshot.schema_hash = schema_hash;

      if(!DataTamerParser::ParseSnapshot(dt_schema, snapshot, callback_number))
      {
        problem = "the payload does not match the schema";
      }
    }
    catch(const std::runtime_error& e)
    {
      problem = e.what();
    }
    if(!problem.empty())
    {
      // report it and go on with the next message
      std::cerr << channel_name << ": skipped a corrupt message: " << problem
                << std::endl;
      continue;
    }

    auto& message_counts = message_counts_per_channel[channel_name];
    for(const auto& series_name : series)
    {
      ++message_counts[series_name];
    }
  }

  // display the counted data samples
  for(const auto& [channel_name, msg_counts] : message_counts_per_channel)
  {
    std::cout << channel_name << ":" << std::endl;
    for(const auto& [name, count] : msg_counts)
    {
      std::cout << "   " << name << ":" << count << std::endl;
    }
  }
  return 0;
}
