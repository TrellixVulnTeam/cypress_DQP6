/*
 *  Cypress -- C++ Spiking Neural Network Simulation Framework
 *  Copyright (C) 2016  Andreas Stöckel
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <chrono>
#include <iostream>
#include <iomanip>
#include <map>

#include <stdlib.h>

#include <cypress/backend/nest/sli.hpp>
#include <cypress/core/neurons.hpp>
#include <cypress/core/network.hpp>

namespace cypress {
namespace sli {
namespace {

/**
 * Converts two time stamps to a duration in seconds.
 *
 * @param t1 is the first time stamp.
 * @param t2 is the second time stamp.
 * @return the duration in seconds.
 */
template <typename T>
float to_seconds(const T &t1, const T &t2)
{
	return std::chrono::duration<float>(t2 - t1).count();
}

/**
 * Allows to serialise a vector of floats as SLI array.
 */
std::ostream &operator<<(std::ostream &os, const std::vector<float> &ls)
{
	bool first = true;
	os << "[";
	for (float f : ls) {
		if (!first) {
			os << " ";
		}
		os << std::showpoint << f;
		first = false;
	}
	return os << "]";
}

/**
 * Wrapper used for printing SLI key-value pairs.
 */
template <typename V>
struct KeyValue {
	std::string key;
	V value;

	KeyValue(const std::string &key, const V &value) : key(key), value(value) {}

	friend std::ostream &operator<<(std::ostream &os, const KeyValue &p)
	{
		return os << "/" << p.key << " " << std::showpoint << p.value << " ";
	}
};

template <typename V>
KeyValue<V> kv(const std::string &key, const V &value)
{
	return KeyValue<V>(key, value);
}

/**
 * Writes the parameters for a IfCondExp neuron.
 */
void write_parameters(std::ostream &os, const IfCondExpParameters &params)
{
	os << "<< " << kv("C_m", params.cm() * 1e3)  // nF -> pF
	   << kv("g_L", params.g_leak() * 1e3)       // uS -> nS
	   << kv("tau_syn_ex", params.tau_syn_E())
	   << kv("tau_syn_in", params.tau_syn_I())
	   << kv("t_ref", params.tau_refrac()) << kv("V_reset", params.v_reset())
	   << kv("V_th", params.v_thresh()) << kv("E_L", params.v_rest())
	   << kv("V_m", params.v_rest()) << kv("E_ex", params.e_rev_E())
	   << kv("E_in", params.e_rev_I()) << kv("I_e", params.i_offset() * 1e3)
	   << ">>";  // nA -> pA
}

/**
 * Writes the parameters for a EifCondExpIsfaIsta (AdEx) neuron.
 */
void write_parameters(std::ostream &os,
                      const EifCondExpIsfaIstaParameters &params)
{
	os << "<< " << kv("C_m", params.cm() * 1e3)
	   << kv("g_L", params.g_leak() * 1e3)
	   << kv("tau_syn_ex", params.tau_syn_E())
	   << kv("tau_syn_in", params.tau_syn_I())
	   << kv("t_ref", params.tau_refrac()) << kv("tau_w", params.tau_w())
	   << kv("V_reset", params.v_reset()) << kv("V_th", params.v_thresh())
	   << kv("E_L", params.v_rest()) << kv("V_m", params.v_rest())
	   << kv("E_ex", params.e_rev_E()) << kv("E_in", params.e_rev_I())
	   << kv("I_e", params.i_offset() * 1e3) << kv("a", params.a() * 1e3)
	   << kv("b", params.b() * 1e3) << kv("Delta_T", params.delta_T()) << ">>";
}

/**
 * Writes the parameters for a SpikeSourceArrayParameters neuron.
 */
void write_parameters(std::ostream &os,
                      const SpikeSourceArrayParameters &params)
{
	os << "<< /allow_offgrid_spikes true /spike_times " << params.spike_times()
	   << " >>";
}

/**
 * Structure containing information about the signals that should be recorded
 * from a neuron.
 */
struct RecordInfo {
	bool spikes, v, gsyn_exc, gsyn_inh;
	bool any() const { return spikes || v || gsyn_exc || gsyn_inh; }
};

RecordInfo record_info(const Neuron<SpikeSourceArray> &n)
{
	return {n.signals().is_recording_spikes(), false, false, false};
}

RecordInfo record_info(const Neuron<IfCondExp> &n)
{
	return {n.signals().is_recording_spikes(), n.signals().is_recording_v(),
	        n.signals().is_recording_gsyn_exc(),
	        n.signals().is_recording_gsyn_inh()};
}

RecordInfo record_info(const Neuron<EifCondExpIsfaIsta> &n)
{
	return {n.signals().is_recording_spikes(), n.signals().is_recording_v(),
	        n.signals().is_recording_gsyn_exc(),
	        n.signals().is_recording_gsyn_inh()};
}

/**
 * Functions used to create the various recorders.
 */

size_t create_recorder(std::ostream &os, const std::string &name, size_t &gid)
{
	os << "/" << name << " << /withtime true /withgid false /to_file false "
	                     "/to_memory true >> Create\n";
	return ++gid;
}

size_t create_multimeter(std::ostream &os, const std::string &name, size_t &gid)
{
	os << "/multimeter << /withtime true /withgid false /to_file false "
	      "/to_memory true /record_from [/"
	   << name << "] >> Create\n";
	return ++gid;
}

/**
 * Creates the population instances.
 */
template <typename T>
void write_population(std::ostream &os, const std::string &s,
                      const T &population, size_t &gid)
{
	// Generate pop.size() object instances, directly set the parameters if they
	// are homogeneous
	os << "/" << s << " " << population.size() << " ";
	if (population.homogeneous_parameters()) {
		write_parameters(os, population.parameters());
	}
	os << " Create\n";

	// The parameters are not homogeneous, write them for each population
	// individually.
	if (!population.homogeneous_parameters()) {
		for (size_t i = 0; i < population.size(); i++) {
			os << (gid + i + 1) << " ";
			write_parameters(os, population[i].parameters());
			os << " SetStatus\n";
		}
	}

	// Advance the current global object id
	gid = gid + population.size();
}

void write_populations(std::ostream &os,
                       const std::vector<PopulationBase> &populations,
                       size_t &gid, std::map<size_t, size_t> &pop_gid_map)
{
	// Write the population definitions
	for (size_t i = 0; i < populations.size(); i++) {
		// Skip zero-sized populations
		const auto &population = populations[i];
		if (population.size() == 0) {
			continue;
		}

		// Remember the association between the population id and
		pop_gid_map.emplace(i, gid + 1);

		// Write the population data
		if (&population.type() == &IfCondExp::inst()) {
			write_population(os, "iaf_cond_exp",
			                 Population<IfCondExp>(population), gid);
		}
		else if (&population.type() == &EifCondExpIsfaIsta::inst()) {
			write_population(os, "aeif_cond_alpha",
			                 Population<EifCondExpIsfaIsta>(population), gid);
		}
		else if (&population.type() == &SpikeSourceArray::inst()) {
			write_population(os, "spike_generator",
			                 Population<SpikeSourceArray>(population), gid);
		}
		else {
			throw ExecutionError("Population type " + population.type().name +
			                     " not supported by the NEST backend!");
		}
	}
}

void write_connections(std::ostream &os,
                       const std::vector<ConnectionDescriptor> &descrs,
                       size_t &, const std::map<size_t, size_t> &pop_gid_map)
{
	// TODO: Here we blindly generate all connections. NEST directly supports
	// most of our connection types, so this code could be much improved

	// Vector containing all connection objects
	std::vector<Connection> connections = instantiate_connections(descrs);
	for (const auto &connection : connections) {
		const auto it_src = pop_gid_map.find(connection.psrc);
		const auto it_tar = pop_gid_map.find(connection.ptar);
		if (it_src == pop_gid_map.end() || it_tar == pop_gid_map.end()) {
			continue;
		}
		os << (it_src->second + connection.n.src) << " "
		   << (it_tar->second + connection.n.tar) << " " << std::showpoint
		   << connection.n.synapse.weight * 1e3 << " "
		   << std::showpoint  // uS -> nS
		   << std::max(0.1f, connection.n.synapse.delay) << " Connect\n";
	}
}

constexpr int MODALITY_SPIKES = 0;
constexpr int MODALITY_V = 1;
constexpr int MODALITY_GSYN_EXC = 2;
constexpr int MODALITY_GSYN_INH = 3;
const std::vector<std::string> MODALITY_STRS = {"spikes", "V_m", "g_ex",
                                                "g_in"};

struct RecorderInfo {
	PopulationIndex pid;
	NeuronIndex nid;
	size_t gid;
	int modality;
};

std::vector<RecorderInfo> write_recorders(
    std::ostream &os, const std::vector<PopulationBase> &populations,
    size_t &gid, const std::map<size_t, size_t> &pop_gid_map)
{
	std::vector<RecorderInfo> res;
	for (size_t i = 0; i < populations.size(); i++) {
		// Fetch the global object offset for this population object
		const auto it = pop_gid_map.find(i);
		if (it == pop_gid_map.end()) {
			continue;
		}
		const size_t pop_gid_offs = it->second;

		// Fetch the population object andl
		const auto &population = populations[i];
		for (NeuronIndex j = 0; j < NeuronIndex(population.size()); j++) {
			// Check which signals should be recorded for neuron j in the i-th
			// population
			RecordInfo info;
			if (&population.type() == &IfCondExp::inst()) {
				info = record_info(Neuron<IfCondExp>(population[i]));
			}
			else if (&population.type() == &EifCondExpIsfaIsta::inst()) {
				info = record_info(Neuron<EifCondExpIsfaIsta>(population[i]));
			}
			else if (&population.type() == &SpikeSourceArray::inst()) {
				info = record_info(Neuron<SpikeSourceArray>(population[i]));
			}

			// Create a recorder for each signal and connect the source neuron
			// to the recorder
			if (info.spikes) {
				res.push_back({population.pid(), j,
				               create_recorder(os, "spike_detector", gid),
				               MODALITY_SPIKES});
				os << (pop_gid_offs + j) << " " << gid << " Connect\n";
			}
			if (info.v) {
				res.push_back({population.pid(), j,
				               create_multimeter(os, "V_m", gid), MODALITY_V});
				os << gid << " " << (pop_gid_offs + j) << " Connect\n";
			}
			if (info.gsyn_exc) {
				res.push_back({population.pid(), j,
				               create_multimeter(os, "g_ex", gid),
				               MODALITY_GSYN_EXC});
				os << gid << " " << (pop_gid_offs + j) << " Connect\n";
			}
			if (info.gsyn_inh) {
				res.push_back({population.pid(), j,
				               create_multimeter(os, "g_in", gid),
				               MODALITY_GSYN_INH});
				os << gid << " " << (pop_gid_offs + j) << " Connect\n";
			}
		}
	}
	return res;
}

void write_readback_cmds(std::ostream &os,
                         const std::vector<RecorderInfo> &recorder_info)
{
	for (const auto &info : recorder_info) {
		// Write a header containing the important data
		os << "(##cypress_data) = ";
		os << info.pid << " = ";
		os << info.nid << " = ";
		os << info.modality << " =\n";

		// Write the length of the upcomming data block
		os << info.gid << " /n_events get =\n";

		// Dump the data
		if (info.modality == MODALITY_SPIKES) {
			os << info.gid << " /events get /times get {=} forall\n";
		}
		else {
			os << "0 1 " << info.gid << " /n_events get 1 sub { dup "
			   << info.gid
			   << " /events get /times get exch get =only ( ) =only "
			   << info.gid << " /events get /" << MODALITY_STRS[info.modality]
			   << " get exch get = } for\n";
		}
	}
}

std::shared_ptr<Matrix<float>> fetch_data_matrix(PopulationBase pop,
                                                 NeuronIndex nid, size_t len,
                                                 int modality)
{
	auto res = std::make_shared<Matrix<float>>(
	    len, modality == MODALITY_SPIKES ? 1 : 2);
	if (&pop.type() == &SpikeSourceArray::inst() &&
	    modality != MODALITY_SPIKES) {
		throw std::invalid_argument("Invalid modality!");
	}
	pop[nid].signals().data(modality, res);
	return res;
}
}

void write_network(std::ostream &os, const NetworkBase &net, float duration)
{
	std::map<size_t, size_t> pop_gid_map;

	// Create the network, setup recorder
	size_t gid = 0;
	os << "(##cypress_setup) =\n";
	write_populations(os, net.populations(), gid, pop_gid_map);
	write_connections(os, net.connections(), gid, pop_gid_map);
	std::vector<RecorderInfo> recorder_info =
	    write_recorders(os, net.populations(), gid, pop_gid_map);

	// Simulate the network
	os << "(##cypress_simulate_start) =\n";
	os << duration << " Simulate\n";
	os << "(##cypress_simulate_stop) =\n";

	// Issue the commands which output the recorded data after the network
	// executed
	write_readback_cmds(os, recorder_info);

	// Indicate the end of the simulation
	os << "(##cypress_done) =\n";
}

void read_response(std::istream &is, NetworkBase &net, std::ostream &errs)
{
	// States of the internally used state machine
	constexpr int STATE_DEFAULT = 0;
	constexpr int STATE_DATA_PID = 1;
	constexpr int STATE_DATA_NID = 2;
	constexpr int STATE_DATA_MODALITY = 3;
	constexpr int STATE_DATA_LEN = 4;
	constexpr int STATE_DATA = 5;

	// Variables containing the profiling time points
	std::chrono::steady_clock::time_point t_setup;
	std::chrono::steady_clock::time_point t_simulate_start;
	std::chrono::steady_clock::time_point t_simulate_stop;
	std::chrono::steady_clock::time_point t_done;

	// State variables
	int state = STATE_DEFAULT;
	PopulationIndex pid = 0;
	NeuronIndex nid = 0;
	int modality = 0;
	size_t len = 0;
	std::shared_ptr<Matrix<float>> data;
	size_t data_idx = 0;

	// Go through the NEST output line-by-line, ignore everything that does not
	// start with "##" while we are in the default state.
	std::string line;
	while (std::getline(is, line)) {
		// Check for special commands
		if (line.size() >= 2 && line[0] == '#' && line[1] == '#') {
			// Reset the state
			state = STATE_DEFAULT;

			// Handle the time measurement points
			if (line == "##cypress_setup") {
				t_setup = std::chrono::steady_clock::now();
			}
			else if (line == "##cypress_simulate_start") {
				t_simulate_start = std::chrono::steady_clock::now();
			}
			else if (line == "##cypress_simulate_stop") {
				t_simulate_stop = std::chrono::steady_clock::now();
			}
			else if (line == "##cypress_done") {
				t_done = std::chrono::steady_clock::now();
			}
			else if (line == "##cypress_data") {
				state = STATE_DATA_PID;
			}
		}
		// Process the "STATE_DATA*" states
		else if (state != STATE_DEFAULT) {
			size_t idx = 0;
			switch (state) {
				case STATE_DATA_PID:  // Load the population id
					pid = std::stol(line, &idx);
					if (pid < 0 || size_t(pid) >= net.population_count()) {
						throw std::invalid_argument(
						    "Invalid population index!");
					}
					state = STATE_DATA_NID;
					break;
				case STATE_DATA_NID:  // Load the neuron id
					nid = std::stol(line, &idx);
					if (nid < 0 || size_t(nid) >= net[pid].size()) {
						throw std::invalid_argument("Invalid neuron index!");
					}
					state = STATE_DATA_MODALITY;
					break;
				case STATE_DATA_MODALITY:  // Load the recorded modality
					modality = std::stol(line, &idx);
					if (modality < MODALITY_SPIKES ||
					    modality > MODALITY_GSYN_INH) {
						throw std::invalid_argument("Invalid modality!");
					}
					state = STATE_DATA_LEN;
					break;
				case STATE_DATA_LEN:  // Load the data length and create the
					                  // target matrix
					len = std::stoull(line, &idx);
					data = fetch_data_matrix(net[pid], nid, len, modality);
					data_idx = 0;
					if (len > 0) {
						state = STATE_DATA;
					}
					else {
						state = STATE_DEFAULT;
					}
					break;
				case STATE_DATA:  // Write the data entries
					if (modality == MODALITY_SPIKES) {
						(*data)(data_idx, 0) = std::stof(line, &idx);
					}
					else {
						// Use some C pointer magic here to avoid having to
						// substr "line"
						const char *s = line.c_str();
						char *s0 = const_cast<char *>(s);
						char *s1 = const_cast<char *>(s) + line.size();

						// Extract the first column
						(*data)(data_idx, 0) = strtof(s0, &s1);

						// Extract the second column
						s1++;
						s0 = s1;
						s1 = const_cast<char *>(s) + line.size();
						(*data)(data_idx, 1) = strtof(s0, &s1);

						// Calulcate the total number of characters processed
						idx = s1 - s;
					}
					if (++data_idx >= len) {
						state = STATE_DEFAULT;
					}
					break;
			}
			if (idx != line.size()) {
				throw std::invalid_argument(
				    "Unexpected characters at the end of the line!");
			}
		}
		else if (state == STATE_DEFAULT) {
			if (!line.empty()) {
				errs << line << std::endl;
			}
		}
	}

	// Set the network benchmark
	net.runtime({to_seconds(t_setup, t_done),
	             to_seconds(t_simulate_start, t_simulate_stop),
	             to_seconds(t_setup, t_simulate_start),
	             to_seconds(t_simulate_stop, t_done)});
}
}
}
