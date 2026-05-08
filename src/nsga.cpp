// nsga.cpp
// Compile: g++ -O2 -std=c++17 nsga.cpp -o nsga
//
// NSGA-II for Energy Flexibility.

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <random>
#include <limits>
#include <iomanip>
#include <unordered_set>
#include <map>

using namespace std;
// Compile: g++ -O2 -std=c++17 nsga_v2.cpp -o nsga
// NSGA-II for energy flexibility (weekly schedules, 3-tier consumption fill, virtual islanding)

// ---------------- Config ----------------
int HOURS = 24;
int POP_SIZE = 100;
int GENERATIONS = 120;
double MUTATION_RATE = 0.02;
unsigned RANDOM_SEED = 123456789u;

enum ImbalanceMode { IMB_AVAIL = 0, IMB_USED = 1 };
ImbalanceMode GLOBAL_IMB_MODE = IMB_AVAIL;

// ---------------- Types ----------------
class Device {
public:
    string name;
    // weekdayHour[w][h] : kWh when ON for weekday w (0=Mon..6=Sun)
    vector<vector<double>> weekdayHour;
    vector<vector<int>> baseHabit;
    // derived
    vector<int> durationByWeekday;      // count ON hours inferred per weekday
    vector<int> minBlockByWeekday;      // minimal consecutive ON block (in hours) per weekday (0=>no demand)
    int maxSwitches = 12;

    Device(const string &n = "", int hours = 24) : name(n) {
        weekdayHour.assign(7, vector<double>(hours, 0.0));
        baseHabit.assign(7, vector<int>(hours, 0));
        durationByWeekday.assign(7, 0);
        minBlockByWeekday.assign(7, 0);
    }
};

class EnergySource {
public:
    string name;
    bool isGrid = false;
    vector<double> available;   // kWh available at hour h
    vector<double> costPerkWh;
    vector<double> co2PerkWh;
    EnergySource(const string &n = "", int hours = 24) : name(n) {
        available.assign(hours, 0.0);
        costPerkWh.assign(hours, 0.0);
        co2PerkWh.assign(hours, 0.0);
    }
};

class Individual {
public:
    // schedule[w][d][h]
    vector<vector<vector<int>>> schedule;
    // allocation[w][h][s] : how much from source s at weekday w and hour h
    vector<vector<vector<double>>> allocation;

    // objectives (minimize)
    double obj_imbalance = 0.0;
    double obj_cost = 0.0;
    double obj_co2 = 0.0;
    double obj_peakGrid = 0.0;
    double obj_discomfort = 0.0;

    // diagnostics
    double totalLoad = 0.0;
    double totalRenewable = 0.0;
    double totalGrid = 0.0;

    // nsga helpers
    double crowdingDistance = 0.0;
    int rank = 0;

    Individual() {}

    void ensureSizes(int nDevices, int hours, int nSources) {
        if ((int)schedule.size() != 7) schedule.assign(7, vector<vector<int>>(nDevices, vector<int>(hours, 0)));
        for (int w=0; w<7; ++w) {
            if ((int)schedule[w].size() != nDevices) schedule[w].assign(nDevices, vector<int>(hours,0));
            for (int d=0; d<nDevices; ++d) if ((int)schedule[w][d].size() != hours) schedule[w][d].resize(hours,0);
        }
        allocation.assign(7, vector<vector<double>>(hours, vector<double>(nSources, 0.0)));
    }
};

// ---------------- Helpers ----------------
bool parseTimestampToWeekHour(const string &ts, int &weekdayOut, int &hourOut) {
    tm t = {};
    istringstream ss(ts);

    // try full seconds first, then without seconds
    ss >> get_time(&t, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        ss.clear();
        ss.str(ts);
        ss >> get_time(&t, "%Y-%m-%d %H:%M");
        if (ss.fail()) return false;
    }

    // normalization overflow to fill tm_wday
    time_t tt = mktime(&t);
    if (tt == (time_t)-1) {
        // fallback use tm as-is
        weekdayOut = (t.tm_wday + 6) % 7;
        hourOut = t.tm_hour;
        return true;
    }

    // convert: tm_wday 0=Sun -> Mon=0
    weekdayOut = (t.tm_wday + 6) % 7;
    hourOut = t.tm_hour;
    return true;
}

// safe split by delimiter
static vector<string> splitDelim(const string &s, char delim) {
    vector<string> out; 
    string cur; 
    stringstream ss(s);

    while (getline(ss, cur, delim)) out.push_back(cur);

    return out;
}

vector<Device> parseConsumption(const string &filename, int &outHours) {
    outHours = 24;
    int HOURS = outHours;
    
    ifstream ifs(filename);

    // Read the CSV file and build the data structure
    if (!ifs.is_open()) {
        cerr << "Cannot open consumption file: " << filename << "\n";
        return {};
    }

    string header;

    if (!getline(ifs, header)) { cerr << "Empty consumption file\n"; return {}; }

    char delim = (header.find(';') != string::npos) ? ';' : ',';
    vector<string> cols = splitDelim(header, delim);
    int deviceCount = max(0, (int)cols.size() - 2);

    if (deviceCount <= 0) return {};

    vector<Device> devices;
    devices.reserve(deviceCount);

    for (int p = 0; p < deviceCount; ++p) {
        devices.emplace_back("D" + to_string(p + 1), HOURS);
    }

    map<string, vector<vector<double>>> dailyData; // date -> [hour][device] -> kWh
    map<string, int> dateToWeekday; // date -> weekday (0=Mon..6=Sun)
    string line; // buffer for reading lines

    // Read each line of the CSV and populate dailyData and dateToWeekday
    while (getline(ifs, line)) {
        if (line.empty()) continue;

        vector<string> cells = splitDelim(line, delim);
        
        if ((int)cells.size() < 2 + deviceCount) {
            //pad
            // cerr << "Warning: missing columns in line: " << line << endl;
            cells.resize(2+deviceCount);
        }

        string ts = cells[0]; // timestamp
        string datePart = (ts.size() >= 10) ? ts.substr(0, 10) : ""; // extract YYYY-MM-DD

        if (datePart.empty()) continue;

        int wd, hr; // weekday and hour

        if (!parseTimestampToWeekHour(ts, wd, hr)) continue; // Save weekday and hour in each line into wd and hr variables. If parsing fails, skip this line.
        if (hr < 0 || hr >= HOURS) continue; 
        // Initialize the power of device (dailyData) entry for this date if it doesn't exist. This will create a 2D vector of size [HOURS][deviceCount] initialized to 0.0 for this date, and also store the corresponding weekday in dateToWeekday.
        if (dailyData[datePart].empty()) {
            dailyData[datePart].assign(HOURS, vector<double>(deviceCount, 0.0));
            dateToWeekday[datePart] = wd; 
        }

        for (int p = 0; p < deviceCount; ++p) {
            string cell = cells[2 + p];
            for (char &c : cell) if (c == ',') c = '.'; 
            
            double v = 0.0;
            if (!cell.empty()) {
                try { v = stod(cell); } catch(...) { v = 0.0; }
            }
            dailyData[datePart][hr][p] = v;
        }
    }
    ifs.close(); 
    
    // EMA - Exponential Moving Average with time-decay weighting
    // Input: vector of {date, power} for a specific device, weekday, and hour across multiple days
    // Output: single double value representing the EMA, which gives more weight to recent days while still considering older data. This helps to fill in missing values (0.0) in weekdayHour with a more informed estimate based on observed consumption patterns, while allowing the model to adapt to changes in user behavior over time.
    auto calcEMA = [](vector<pair<string, double>>& dataPoints) -> double {
        if (dataPoints.empty()) return 0.0;
        
        // Sort data points by date to ensure correct time-decay weighting (oldest to newest). This is crucial for the EMA calculation to give more weight to recent data. The sorting is done based on the date string, which is in YYYY-MM-DD format, so lexicographical sorting will work correctly.
        sort(dataPoints.begin(), dataPoints.end(), 
             [](const pair<string,double>& a, const pair<string,double>& b) {
                 return a.first < b.first; 
             });

        double sumWeighted = 0.0;
        double totalWeight = 0.0;
        
        // Apply exponential weighting: newest (largest index) -> highest weight
        for (size_t i = 0; i < dataPoints.size(); ++i) {
            double weight = exp(0.5 * i); // exponential growth in weight for more recent data points. The factor (0.5) controls the rate of decay; it can be adjusted based on how quickly the model needs to adapt to changes in user behavior. A higher factor will give even more weight to recent data, while a lower factor will make the EMA more stable and less sensitive to recent changes.
            sumWeighted += dataPoints[i].second * weight;
            totalWeight += weight;
        }
        return sumWeighted / totalWeight;
    };

    // Process the collected daily data to fill in the weekdayHour for each device 
    // Compute the durationByWeekday and minBlockByWeekday for each device. This involves:
    // 1. Organizing the data into powerBucket and daySeqs for each device and weekday.
    // 2. Calculating the EMA for observed hours and filling in missing hours based on the 4-tier logic 
    // (Tier1: same hour, Tier2: same session Tier 3: same day, Tier4: no demand).
    // 3. Analyzing the binary sequences of ON/OFF hours to determine the minimum block size of continuous ON hours for each device and weekday.
    for (int p = 0; p < deviceCount; ++p) {
        // powerBucket: 7 weekdays x HOURS hours -> list of {date, power} for that device at that weekday and hour across multiple days.
        vector<vector<vector<pair<string, double>>>> powerBucket(7, vector<vector<pair<string, double>>>(HOURS));
        vector<vector<vector<int>>> daySeqs(7); // binary sequences of ON/OFF hours for each day, used to analyze patterns and determine minBlockByWeekday

        for (const auto& kv : dailyData) {
            const string& dateKey = kv.first;
            const auto& hoursMatrix = kv.second;
            int wd = dateToWeekday[dateKey];
            vector<int> binarySeq(HOURS, 0); 
            
            for (int hr = 0; hr < HOURS; ++hr) {
                double v = hoursMatrix[hr][p];
                if (v > 0.0) {
                    powerBucket[wd][hr].push_back({dateKey, v}); 
                    binarySeq[hr] = 1;
                }
            }
            daySeqs[wd].push_back(binarySeq);
        }

        // Calculate EMA and fill in weekdayHour with 4-tier logic (Tier1: same hour, Tier2: same session, Tier3: same day, Tier4: no demand)
        for (int w = 0; w < 7; ++w) {
            vector<pair<string, double>> dailyDataList;       // Power data of all hours in that weekday across multiple days --> average power for Tier 3
            vector<pair<string, double>> segmentDataList[4];  // Power data for 4 segments of the day (0-5h, 6-11h, 12-17h, 18-23h) in that weekday across multiple days --> power for Tier 2

            for (int h = 0; h < HOURS; ++h) {
                if (!powerBucket[w][h].empty()) {
                    // Tier 1: Call calcEMA to compute the EMA for this hour based on the observed power values across multiple days. 
                    devices[p].weekdayHour[w][h] = calcEMA(powerBucket[w][h]);
                    devices[p].baseHabit[w][h] = 1;

                    int seg = h / 6; // determine segment (0-5h: seg=0, 6-11h: seg=1, 12-17h: seg=2, 18-23h: seg=3)
                    
                    // lists of power values used for filling in missing hours based on the 4-tier logic.
                    for (auto& item : powerBucket[w][h]) {
                        dailyDataList.push_back(item); // for the entire day
                        segmentDataList[seg].push_back(item); // for each segment of the day
                    }
                } else {
                    devices[p].weekdayHour[w][h] = 0.0;
                    devices[p].baseHabit[w][h] = 0;
                }
            }

            if (!dailyDataList.empty()) {
                double avgDailyEMA = calcEMA(dailyDataList); // Calculate EMA of the entire day for this device and weekday, used for Tier 3 filling when there is no data for the specific hour and segment (Tier 1 and 2).
                double segmentEMA[4] = {0.0}; 
                
                // Calculate EMA for each of the 4 segments of the day, used for Tier 2 filling when there is no data for the specific hour but there is data in the same segment (Tier 1).
                for (int i=0; i<4; ++i) {
                    if (!segmentDataList[i].empty()) segmentEMA[i] = calcEMA(segmentDataList[i]);
                }

                for (int h = 0; h < HOURS; ++h) {
                    if (devices[p].weekdayHour[w][h] == 0.0) {
                        int seg = h / 6; 
                        
                        if (!segmentDataList[seg].empty()) {
                            // Tier 2: If there is no data for the specific hour (Tier 1) but there is data in the same segment of the day across multiple days, use the EMA of that segment to fill in the missing value for that hour. 
                            devices[p].weekdayHour[w][h] = segmentEMA[seg];
                        } else {
                            // Tier 3: If there is no data for the specific hour (Tier 1) and no data in the same segment (Tier 2), but there is data for that entire weekday across multiple days, use the EMA of the entire day to fill in the missing value for that hour. This allows us to still provide an informed estimate for hours with no specific data, based on the overall consumption pattern of that device on that weekday.
                            devices[p].weekdayHour[w][h] = avgDailyEMA; 
                        }
                    }
                }
            }
            // Tier 4: If there is no data for that device and weekday across multiple days, the weekdayHour values will remain 0.0, which indicates no demand for that device on that weekday. The scheduling algorithm will then treat this device as unavailable on that day and will not schedule it ON, ensuring that we do not create unrealistic schedules by turning ON devices that have no observed demand on that day.
        }

        // Analyze the binary sequences of ON/OFF hours (daySeqs) --> the minimum block size of continuous ON hours (minBlockByWeekday) for each device and weekday.
        for (int w = 0; w < 7; ++w) {
            if (daySeqs[w].empty()) {
                devices[p].minBlockByWeekday[w] = 0;
                continue;
            }

            vector<int> dailyTotalHours; // total ON hours in each day for that weekday across multiple days.
            vector<int> allBlockLengths; // lengths of all continuous ON blocks for that device and weekday across multiple days.

            for (const auto& seq : daySeqs[w]) {
                int hoursInDay = 0; // total ON hours in the current day.
                int currentRun = 0; // length of the current continuous ON block.
                
                for (int bit : seq) {
                    if (bit == 1) {
                        hoursInDay++;
                        currentRun++;
                    } else {
                        if (currentRun > 0) allBlockLengths.push_back(currentRun);
                        currentRun = 0;
                    }
                }
                if (currentRun > 0) allBlockLengths.push_back(currentRun);
                if (hoursInDay > 0) dailyTotalHours.push_back(hoursInDay);
            }

            // 1. Calculate MIN BLOCK LENGTH (minBlockByWeekday) by Mode 
            if (!allBlockLengths.empty()) {
                map<int, int> freq; // frequency map to count how many times each block length occurs across all days for that device and weekday.
                
                // Counting how many times each length appears. 
                for (int len : allBlockLengths) freq[len]++;

                int modeLen = 0, maxFreq = 0; // block length that occurs most frequently, and the frequency of that mode.
                
                // Finding the mode of block lengths by iterating through the frequency map. 
                for (auto const& kv : freq) {
                    if (kv.second > maxFreq) { maxFreq = kv.second; modeLen = kv.first; }
                }
                devices[p].minBlockByWeekday[w] = modeLen;
            }

            // 2. Calculate DURATION by Mode 
            if (!dailyTotalHours.empty()) {
                map<int, int> freq; // frequency map to count how many times each total daily ON hour count occurs across multiple days for that device and weekday.

                // Counting how many times each total daily ON hour count appears across multiple days for that device and weekday.
                for (int hrs : dailyTotalHours) freq[hrs]++;

                int modeDur = 0, maxFreq = 0; // total daily ON hour count that occurs most frequently, and the frequency of that mode.

                // Finding the mode of total daily ON hour counts by iterating through the frequency map.
                for (auto const& kv : freq) {
                    if (kv.second > maxFreq) { maxFreq = kv.second; modeDur = kv.first; }
                }
                devices[p].durationByWeekday[w] = modeDur;
            } else {
                devices[p].durationByWeekday[w] = 0;
            }
        }
    } 

    return devices;
}

// parse generation CSV: rows: DateTime;Grid_kWh;Grid_Cost;Grid_CO2;A_kWh;A_Cost;A_CO2;...
vector<EnergySource> parseGeneration(const string &filename, int expectedHours) {
    ifstream ifs(filename);
    if (!ifs.is_open()) {
        cerr << "Cannot open generation file: " << filename << "\n";
        return {};
    }
    string header;
    if (!getline(ifs, header)) return {};
    char delim = (header.find(';') != string::npos) ? ';' : ',';
    // read rows
    vector<vector<string>> rows;
    string line;
    while (getline(ifs, line)) {
        if (line.empty()) continue;
        vector<string> cells = splitDelim(line, delim);
        if ((int)cells.size() < 4) continue;
        rows.push_back(cells);
    }
    if (rows.empty()) return {};

    // 1. Parse header to detect energy source names, and assign default names if not provided. The expected format is: DateTime;Grid_kWh;Grid_Cost;Grid_CO2;A_kWh;A_Cost;A_CO2;B_kWh;B_Cost;B_CO2;...
    vector<string> headerCells = splitDelim(header, delim);
    int tokens = headerCells.size();
    int triples = max(1, (tokens - 1) / 3);

    vector<string> names;
    for (int t = 0; t < triples; ++t) {
        int idx = 1 + t * 3; 
        string sourceName = "Source_" + to_string(t); 

        if (idx < (int)headerCells.size()) {
            string colName = headerCells[idx];
            size_t pos = colName.find('_');
            if (pos != string::npos) {
                sourceName = colName.substr(0, pos); 
            } else {
                sourceName = colName;
            }
        }
        names.push_back(sourceName);
    }

    // 2. Initialize EnergySource objects based on the parsed names and expected hours, and detect grid 
    vector<EnergySource> sources;
    bool gridFound = false;
    for (int t = 0; t < triples; ++t) {
        sources.emplace_back(names[t], expectedHours);
        
        string lowerName = names[t];
        transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName == "grid") {
            sources.back().isGrid = true;
            gridFound = true;
        }
    }
    // detect grid by name if header provided? We used default names, allow user to rename later.
    if (!gridFound && !sources.empty()) {
        sources[0].isGrid = true;
    }

    int rowsToUse = min((int)rows.size(), expectedHours);
    for (int r=0; r<rowsToUse; ++r) {
        auto &cells = rows[r];
        for (int s=0; s<triples; ++s) {
            int idx = 1 + s*3;
            double avail=0.0, cost=0.0, co2=0.0;
            if (idx < (int)cells.size()) {
                string v = cells[idx]; for (char &c:v) if (c==',') c='.'; try{ avail = stod(v);}catch(...){avail=0.0;}
            }
            if (idx+1 < (int)cells.size()) { string v=cells[idx+1]; for (char &c:v) if (c==',') c='.'; try{ cost = stod(v);}catch(...){cost=0.0;} }
            if (idx+2 < (int)cells.size()) { string v=cells[idx+2]; for (char &c:v) if (c==',') c='.'; try{ co2 = stod(v);}catch(...){co2=0.0;} }
            sources[s].available[r] = avail;
            sources[s].costPerkWh[r] = cost;
            sources[s].co2PerkWh[r] = co2;
        }
    }

    return sources;
}

// ---------------- NSGA utilities ----------------
vector<vector<double>> normalizeObjectivesMatrix(const vector<Individual>& pop) {
    int N = pop.size(); // number of individuals in the population
    int m = 5; // number of objectives
    vector<double> minv(m, numeric_limits<double>::infinity()), maxv(m, -numeric_limits<double>::infinity()); // initialize min and max vectors with m elements

    // Iterate through the population to find the minimum and maximum values for each objective across all individuals
    for (auto &ind : pop) {
        minv[0] = min(minv[0], ind.obj_imbalance);
        minv[1] = min(minv[1], ind.obj_cost);
        minv[2] = min(minv[2], ind.obj_co2);
        minv[3] = min(minv[3], ind.obj_peakGrid);
        minv[4] = min(minv[4], ind.obj_discomfort);
        maxv[0] = max(maxv[0], ind.obj_imbalance);
        maxv[1] = max(maxv[1], ind.obj_cost);
        maxv[2] = max(maxv[2], ind.obj_co2);
        maxv[3] = max(maxv[3], ind.obj_peakGrid);
        maxv[4] = max(maxv[4], ind.obj_discomfort);
    }

    vector<vector<double>> out(N, vector<double>(m,0.0)); // initialize the output matrix for normalized objectives with N rows and m columns

    // Normalize the objectives for each individual
    for (int i=0; i<N; ++i) {
        double vals[5] = { pop[i].obj_imbalance, pop[i].obj_cost, pop[i].obj_co2, pop[i].obj_peakGrid, pop[i].obj_discomfort }; // array of the objective values for the current individual

        // Normalize each objective value to the range [0, 1] using min-max normalization
        for (int j=0;j<m;++j) {
            double denom = maxv[j] - minv[j];
            if (denom < 1e-12) denom = 1.0;
            out[i][j] = (vals[j] - minv[j]) / denom;
        }
    }
    return out;
}

// If a is no worse than b in all objectives and better in at least one objective -> true if a dominates b, and false otherwise.
bool dominates(const vector<double>& a, const vector<double>& b) {
    bool strictly=false;
    for (size_t i=0;i<a.size();++i) {
        if (a[i] > b[i] + 1e-12) return false;
        if (a[i] < b[i] - 1e-12) strictly = true;
    }
    return strictly;
}

vector<vector<int>> fastNonDominatedSort(const vector<Individual>& pop, const vector<vector<double>>& norm) {
    int N = pop.size(); // number of individuals in the population
    vector<int> domCount(N,0); // number of individuals dominating or not be dominated by current individual
    vector<vector<int>> dominated(N); // list of individuals dominated by current individual, with N elements initialized to empty vectors
    vector<vector<int>> fronts; // list of non-dominated fronts, where each front is a vector of indices of individuals in that front
    fronts.emplace_back(); // start with the first front (index 0) as an empty vector
    
    for (int p=0; p<N; ++p) {
        for (int q=0; q<N; ++q) {
            if (p==q) continue;
            if (dominates(norm[p], norm[q])) dominated[p].push_back(q);
            else if (dominates(norm[q], norm[p])) domCount[p]++;
        }
        // If domCount[p] is zero, it means that individual p is not dominated by any other individual, so we add it to the first front (fronts[0]).
        if (domCount[p]==0) fronts[0].push_back(p);
    }

    int idx=0; // index to keep track of the current front being processed

    while (idx < (int)fronts.size()) {
        vector<int> next; // list of individuals for the next front (fronts[idx+1]) that are dominated by individuals in the current front (fronts[idx])

        for (int p : fronts[idx]) {
            for (int q : dominated[p]) {
                domCount[q]--;
                if (domCount[q]==0) next.push_back(q);
            }
        }
        if (!next.empty()) fronts.push_back(next);
        ++idx;
    }
    return fronts;
}

void assignCrowdingDistance(vector<Individual>& pop, const vector<int>& front, const vector<vector<double>>& norm) {
    int l = front.size(); // number of individuals index in the current front 

    // If l=0, it means there are no individuals in this front, 
    // so we can return early without doing any calculations
    if (l==0) return; 

    // Initialize the crowding distance for all individuals in the current front to 0.0
    for (int idx : front) pop[idx].crowdingDistance = 0.0;

    int m = norm[0].size(); // number of objectives

    // For each objective, we sort the individuals in the current front based on their normalized objective values for that objective.
    for (int obj=0; obj<m; ++obj) {
        vector<int> tmp = front; // temporary vector to hold the list of indices of individuals in the current front for sorting based on the current objective
        
        // Sort ascending the individuals in the current front based on their normalized objective values for the current objective (obj)
        sort(tmp.begin(), tmp.end(), [&](int a,int b){ return norm[a][obj] < norm[b][obj]; });

        // Set the crowding distance of the first and last individuals in the sorted list to infinity
        pop[tmp.front()].crowdingDistance = numeric_limits<double>::infinity();
        pop[tmp.back()].crowdingDistance  = numeric_limits<double>::infinity();
        
        double fmin = norm[tmp.front()][obj]; // minimum normalized objective value for the current objective of the mininum objective value individual in the current front
        double fmax = norm[tmp.back()][obj]; // maximum normalized objective value for the current objective of the maximum objective value individual in the current front

        // Skip the crowding distance calculation for this objective to avoid division by zero. 
        // This means that if all individuals in the current front have very similar normalized objective values for this objective 
        // -> not consider this objective for crowding distance calculation, as it does not contribute to differentiating between individuals in terms of their crowding distance
        if (fabs(fmax - fmin) < 1e-12) continue;

        // For each individual in the sorted list (except the first and last), 
        // calculate the crowding distance contribution for this objective by taking the difference between the normalized objective values of the next and previous individuals in the sorted list, 
        // and dividing it by the range of normalized objective values (fmax - fmin) for this objective
        for (int i=1;i<(int)tmp.size()-1;++i) {
            pop[tmp[i]].crowdingDistance += (norm[tmp[i+1]][obj] - norm[tmp[i-1]][obj]) / (fmax - fmin);
        }
    }
}

// crossover: Heuristic Device-level Uniform Crossover to preserve device operation patterns and avoid creating unrealistic schedules by cutting ON blocks.
pair<Individual,Individual> crossover(const Individual &p1, const Individual &p2, mt19937 &rng) {
    Individual c1 = p1, c2 = p2; // start with parents' schedules and then swap entire device blocks based on uniform crossover logic
    uniform_real_distribution<> urd(0.0, 1.0);
    int nDevices = p1.schedule[0].size(); // number of devices, schedule is [7][nDevices][HOURS]
    
    // For each device and weekday, with a 50% probability, swap the entire 24-hour schedule of that device for that weekday between the two parents 
    for (int w = 0; w < 7; ++w) {
        for (int d = 0; d < nDevices; ++d) {
            // Uniform Crossover with 50% probability, swap the entire 24h schedule of device d at weekday w between p1 and p2. This preserves realistic operation patterns and avoids creating unrealistic schedules by cutting ON blocks.
            if (urd(rng) < 0.5) {
                // c1 receives entire 24h block of p2
                c1.schedule[w][d] = p2.schedule[w][d];
                // c2 receives entire 24h block of p1
                c2.schedule[w][d] = p1.schedule[w][d];
            }
        }
    }
    return {c1, c2};
}

// sanitize schedule: ensure not turning ON when device has no demand on that weekday (Tier3) and not ON at hour with zero power (Tier1)
void sanitizeSchedule(Individual &ind, const vector<Device>& devices) {
    int nD = devices.size();
    for (int w=0; w<7; ++w) {
        for (int d=0; d<nD; ++d) {
            // if device has no demand that weekday -> zero out
            if (devices[d].durationByWeekday[w] == 0 && devices[d].minBlockByWeekday[w] == 0) {
                for (int h=0; h<HOURS; ++h) ind.schedule[w][d][h] = 0;
            } else {
                for (int h=0; h<HOURS; ++h) {
                    if (devices[d].weekdayHour[w][h] <= 0.0) {
                        // allowed to set ON only if Tier2 inference allowed (i.e., minBlockByWeekday[w]>0)
                        if (devices[d].minBlockByWeekday[w] == 0) ind.schedule[w][d][h] = 0;
                        // else keep it; evaluation will substitute inferred average power
                    }
                }
            }
        }
    }
}

// mutate: Heuristic Block-Based Mutation (Trượt khối liên tục)
// Detect continuous ON blocks for each device and weekday, and with a certain mutation probability, attempt to move the entire block to a new valid position within the same day. This preserves the continuity of operation and total running time of devices, while allowing exploration of different scheduling configurations.
void mutateIndividual(Individual &ind, const vector<Device>& devices, mt19937 &rng) {
    uniform_real_distribution<> urd(0.0, 1.0);
    int nD = devices.size();
    
    for (int w = 0; w < 7; ++w) {
        for (int d = 0; d < nD; ++d) {
            // Pass out early if device has no demand for that weekday (Tier3) to avoid unnecessary processing and to ensure we do not accidentally turn ON a device that should be OFF on that day.
            if (devices[d].durationByWeekday[w] == 0 && devices[d].minBlockByWeekday[w] == 0) continue;

            // 1. Identify all continuous ON blocks for this device and weekday in the individual's schedule.
            // Store them as pairs of {start_hour, length}.
            vector<pair<int, int>> blocks; 
            int h = 0;

            while (h < HOURS) {
                if (ind.schedule[w][d][h] == 1) {
                    int start = h;
                    while (h < HOURS && ind.schedule[w][d][h] == 1) h++;
                    blocks.push_back({start, h - start}); // Lưu lại giờ bắt đầu và độ dài khối
                } else {
                    h++;
                }
            }

            // 2. For each identified block, with a certain mutation probability, attempt to move the entire block to a new valid position within the same day.
            for (auto &blk : blocks) {
                // If this block is hit for mutation
                if (urd(rng) < MUTATION_RATE) {
                    int len = blk.second;
                    int old_start = blk.first;

                    // Set current block to OFF (0) to free up the hours for searching new positions
                    for (int i = 0; i < len; ++i) ind.schedule[w][d][old_start + i] = 0;

                    // 3. Find all valid start positions for the block
                    vector<int> valid_starts;
                    // Loop to find new start position (leave 'len' hours at the end of the day to avoid going out of bounds)
                    for (int st = 0; st <= HOURS - len; ++st) {
                        if (st == old_start) continue;

                        bool can_fit = true;

                        for (int i = 0; i < len; ++i) {
                            int curr_h = st + i;
                            // Condition 1: The hour must be valid (the user has a habit or the system allows it)
                            bool is_allowed = (devices[d].weekdayHour[w][curr_h] > 0.0 || devices[d].minBlockByWeekday[w] > 0);
                            // Condition 2: It must not overlap with another ON block that is already present
                            bool is_empty = (ind.schedule[w][d][curr_h] == 0);
                            
                            if (!is_allowed || !is_empty) {
                                can_fit = false;
                                break; 
                            }
                        }

                        // If all hours in the block can fit, add this start position to valid_starts
                        if (can_fit) valid_starts.push_back(st);
                    }

                    // 4. Move the block to a new valid position
                    if (!valid_starts.empty()) {
                        // Choose randomly 1 place to drop the block
                        uniform_int_distribution<> distStart(0, valid_starts.size() - 1);
                        int new_start = valid_starts[distStart(rng)];
                        
                        // Write the block to the new position
                        for (int i = 0; i < len; ++i) ind.schedule[w][d][new_start + i] = 1;

                        // Update block position in the list (not strictly necessary since we won't use it again, but for consistency)
                        blk.first = new_start;
                    } else {
                        // If there are no valid positions available -> Revert, set back to the old position
                        for (int i = 0; i < len; ++i) ind.schedule[w][d][old_start + i] = 1;
                    }
                }
            } // end loop blocks
        }
    }
    
    // After mutation, we should sanitize the schedule to ensure no invalid ON hours were introduced, especially for devices with strict constraints (Tier1 and Tier3).
    sanitizeSchedule(ind, devices);
}

// Evaluate one individual: aggregate objectives across 7 weekdays (sum across days)
void evaluateIndividual(Individual &ind, const vector<Device>& devices, const vector<EnergySource>& sources) {
    int nD = devices.size();
    int S = sources.size();
    ind.ensureSizes(nD, HOURS, S);
    // reset objectives
    ind.obj_imbalance = ind.obj_cost = ind.obj_co2 = ind.obj_peakGrid = ind.obj_discomfort = 0.0;
    ind.totalLoad = ind.totalRenewable = ind.totalGrid = 0.0;
    // evaluate per weekday
    for (int w=0; w<7; ++w) {
        // prepare allocation for this weekday
        vector<vector<double>> alloc(HOURS, vector<double>(S, 0.0));
        vector<double> load(HOURS, 0.0);
        // compute load
        for (int d=0; d<nD; ++d) {
            for (int h=0; h<HOURS; ++h) {
                if (ind.schedule[w][d][h]) {
                    double power = devices[d].weekdayHour[w][h];
                    if (power > 0.0) {
                        load[h] += power;
                    } else {
                        ind.schedule[w][d][h] = 0;
                    }
                }
            }
        }

        // compute renewable availability (non-grid)
        int gridIndex = -1;
        for (int s=0;s<S;++s) if (sources[s].isGrid || sources[s].name=="Grid") { gridIndex = s; break; }
        if (gridIndex == -1 && S>0) gridIndex=0;

        // per-hour dispatch
        double sumAbsImb = 0.0;
        double totCost = 0.0, totCO2=0.0, totGridImport=0.0, peakGrid=0.0, totLoad=0.0, totRenew=0.0;
        for (int h=0; h<HOURS; ++h) {
            double renewAvail = 0.0;
            for (int s=0;s<S;++s) if (s!=gridIndex) renewAvail += (h < (int)sources[s].available.size() ? sources[s].available[h] : 0.0);
            totRenew += renewAvail;
            double load_h = load[h];
            totLoad += load_h;

            // Imbalance
            double imb_h = 0.0;
            if (GLOBAL_IMB_MODE == IMB_AVAIL) imb_h = fabs(load_h - renewAvail);
            else { // IMB_USED: depends on actual used renewable (we will prefer to use renewable first)
                // simulate allocation to compute renewable used
                double rem = load_h;
                double renewUsed = 0.0;
                // allocate renewable by descending avail
                vector<int> idx;
                for (int s=0;s<S;++s) if (s!=gridIndex) idx.push_back(s);
                sort(idx.begin(), idx.end(), [&](int a,int b){
                    double aa = (h < (int)sources[a].available.size()) ? sources[a].available[h] : 0.0;
                    double bb = (h < (int)sources[b].available.size()) ? sources[b].available[h] : 0.0;
                    return aa > bb;
                });
                for (int sidx : idx) {
                    double avail = (h < (int)sources[sidx].available.size()) ? sources[sidx].available[h] : 0.0;
                    double use = min(rem, avail);
                    renewUsed += use;
                    rem -= use;
                    if (rem <= 1e-12) break;
                }
                imb_h = fabs(load_h - renewUsed);
            }
            sumAbsImb += imb_h;

            // Dispatch: non-grid first then grid
            double remaining = load_h;
            vector<int> idx;
            for (int s=0;s<S;++s) if (s!=gridIndex) idx.push_back(s);
            sort(idx.begin(), idx.end(), [&](int a,int b){
                double aa = (h < (int)sources[a].available.size()) ? sources[a].available[h] : 0.0;
                double bb = (h < (int)sources[b].available.size()) ? sources[b].available[h] : 0.0;
                return aa > bb;
            });
            for (int sidx : idx) {
                double avail = (h < (int)sources[sidx].available.size()) ? sources[sidx].available[h] : 0.0;
                double use = min(remaining, avail);
                if (use > 1e-12) {
                    alloc[h][sidx] = use;
                    if (h < (int)sources[sidx].costPerkWh.size()) totCost += use * sources[sidx].costPerkWh[h];
                    if (h < (int)sources[sidx].co2PerkWh.size()) totCO2 += use * sources[sidx].co2PerkWh[h];
                    remaining -= use;
                }
                if (remaining <= 1e-12) break;
            }
            double gridUse = 0.0;
            if (remaining > 1e-12 && gridIndex >= 0) {
                // if still remaining demand, allocate from grid
                double gAvail = (h < (int)sources[gridIndex].available.size()) ? sources[gridIndex].available[h] : 0.0;
                double use = min(remaining, gAvail);
                if (use > 1e-12) {
                    alloc[h][gridIndex] = use;
                    if (h < (int)sources[gridIndex].costPerkWh.size()) totCost += use * sources[gridIndex].costPerkWh[h];
                    if (h < (int)sources[gridIndex].co2PerkWh.size()) totCO2 += use * sources[gridIndex].co2PerkWh[h];
                    remaining -= use;
                    gridUse += use;
                }
            }
            if (remaining > 1e-12 && gridIndex >= 0) {
                double use = remaining;
                alloc[h][gridIndex] += use;
                if (h < (int)sources[gridIndex].costPerkWh.size()) totCost += use * sources[gridIndex].costPerkWh[h];
                if (h < (int)sources[gridIndex].co2PerkWh.size()) totCO2 += use * sources[gridIndex].co2PerkWh[h];
                gridUse += use;
                remaining = 0.0;
            }
            totGridImport += gridUse;
            peakGrid = max(peakGrid, gridUse);
        } // end hour loop

        // discomfort: Hamming-like across week-day-device; here we compute per-weekday and average across devices/weekdays
        double discomfortSum = 0.0;
        for (int d=0; d<nD; ++d) {
            // preferred binary vector from Tier1 (weekdayHour>0)
            // vector<int> pref(HOURS,0);
            // for (int h=0; h<HOURS; ++h) if (devices[d].weekdayHour[w][h] > 0.0) pref[h] = 1;
            
            // Missing ON hours
            int prefCount = devices[d].durationByWeekday[w];
            int onCount=0;

            for (int h=0; h<HOURS; ++h) if (ind.schedule[w][d][h]) ++onCount;

            double missingFraction = 0.0;

            if (prefCount>0) {
                // int miss=0;
                // for (int h=0; h<HOURS; ++h) if (pref[h]==1 && ind.schedule[w][d][h]==0) ++miss;
                // missingFraction = double(miss)/double(prefCount);
                missingFraction = double(abs(prefCount - onCount)) / double(prefCount);
            }

            // Excess switch times
            int switches=0;

            for (int h=1; h<HOURS; ++h) if (ind.schedule[w][d][h] != ind.schedule[w][d][h-1]) ++switches;

            double excessSwitchFraction = 0.0;
            if (devices[d].maxSwitches > 0) {
                int ex = max(0, switches - devices[d].maxSwitches);
                excessSwitchFraction = min(1.0, double(ex) / double(devices[d].maxSwitches));
            }

            // Hamming distance fraction
            int hamming=0;

            for (int h=0; h<HOURS; ++h) {
                if (devices[d].baseHabit[w][h] != ind.schedule[w][d][h]) ++hamming;
            }
            double hammingFraction = double(hamming)/double(HOURS); 
            double devDisc = (missingFraction + excessSwitchFraction + hammingFraction) / 3.0;
            discomfortSum += devDisc;
        }
        double avgDiscomfort = (nD>0) ? (discomfortSum / double(nD)) : 0.0;

        // commit weekday-level sums into individual's totals (we sum across days)
        ind.obj_imbalance += sumAbsImb;
        ind.obj_cost += totCost;
        ind.obj_co2 += totCO2;
        ind.obj_peakGrid = max(ind.obj_peakGrid, peakGrid); // peak over the week
        ind.obj_discomfort += avgDiscomfort;

        ind.totalLoad += totLoad;
        ind.totalRenewable += totRenew;
        ind.totalGrid += totGridImport;

        // copy allocation for this weekday
        for (int h=0; h<HOURS; ++h) for (int s=0;s<S;++s) ind.allocation[w][h][s] = alloc[h][s];
    } // end weekday loop
    // optionally normalize discomfort by 7 (we accumulated avg per weekday)
    ind.obj_discomfort /= 7.0;
}

// hash schedule for dedupe
string hashSchedule(const Individual &ind) {
    string out;
    for (int w=0; w<7; ++w) {
        for (auto &row : ind.schedule[w]) for (int b : row) out.push_back(b ? '1' : '0');
        out.push_back('|');
    }
    return out;
}

void filterDuplicatePareto(vector<Individual> &pareto) {
    unordered_set<string> seen;
    vector<Individual> ded;
    for (auto &ind : pareto) {
        string h = hashSchedule(ind);
        if (seen.find(h) == seen.end()) {
            seen.insert(h);
            ded.push_back(ind);
        }
    }
    pareto.swap(ded);
}

// JSON export
string escapeJson(const string &s) {
    string out; out.reserve(s.size()*11/10);
    for (char c : s) {
        if (c=='\\') out += "\\\\";
        else if (c=='\"') out += "\\\"";
        else if (c=='\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

void exportParetoJSON(const vector<Individual>& pareto, const vector<Device>& devices, const vector<EnergySource>& sources, const string &fname) {
    // dedupe by schedule hash (make a copy)
    vector<Individual> copy = pareto;
    filterDuplicatePareto(copy);
    ofstream ofs(fname);
    if (!ofs.is_open()) { cerr << "Cannot write " << fname << "\n"; return; }
    ofs << "{\n \"generated_at\": \"" << __DATE__ << " " << __TIME__ << "\",\n \"pareto\": [\n";
    for (size_t i=0;i<copy.size();++i) {
        const auto &ind = copy[i];
        ofs << "  {\n    \"id\": " << (i+1) << ",\n";
        ofs << "    \"objectives\": {\"imbalance\": " << ind.obj_imbalance
            << ", \"cost\": " << ind.obj_cost << ", \"co2\": " << ind.obj_co2
            << ", \"peakGrid\": " << ind.obj_peakGrid << ", \"discomfort\": " << ind.obj_discomfort << "},\n";
        // schedule as 7 elements, each is devices x HOURS
        ofs << "    \"schedule\": [\n";
        for (int w=0; w<7; ++w) {
            ofs << "      [\n";
            for (int d=0; d<(int)devices.size(); ++d) {
                ofs << "        [";
                for (int h=0; h<HOURS; ++h) {
                    ofs << ind.schedule[w][d][h];
                    if (h+1<HOURS) ofs << ",";
                }
                ofs << "]";
                if (d+1 < (int)devices.size()) ofs << ",";
                ofs << "\n";
            }
            ofs << "      ]";
            if (w+1<7) ofs << ",";
            ofs << "\n";
        }
        ofs << "    ],\n";
        // allocation: 7 x HOURS x sources
        ofs << "    \"allocation\": [\n";
        for (int w=0; w<7; ++w) {
            ofs << "      [\n";
            for (int h=0; h<HOURS; ++h) {
                ofs << "        {";
                for (int s=0; s<(int)sources.size(); ++s) {
                    ofs << "\"" << escapeJson(sources[s].name) << "\":" << ind.allocation[w][h][s];
                    if (s+1 < (int)sources.size()) ofs << ", ";
                }
                ofs << "}";
                if (h+1 < HOURS) ofs << ",";
                ofs << "\n";
            }
            ofs << "      ]";
            if (w+1<7) ofs << ",";
            ofs << "\n";
        }
        ofs << "    ]\n  }";
        if (i+1 < copy.size()) ofs << ",";
        ofs << "\n";
    }
    ofs << " ]\n}\n";
    ofs.close();
    cout << "Wrote Pareto JSON to " << fname << " (deduped size=" << copy.size() << ")\n";
}

// ---------------- Main ----------------
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    string consumptionFile = "../data/consumption.csv";
    string generationFile  = "../data/generation.csv";
    string outPareto       = "../pareto_results.json";

    for (int i=1;i<argc;++i) {
        string a = argv[i];
        if (a=="--cons" && i+1<argc) consumptionFile = argv[++i];
        else if (a=="--gen" && i+1<argc) generationFile = argv[++i];
        else if (a=="--out" && i+1<argc) outPareto = argv[++i];
        else if (a=="--pop" && i+1<argc) POP_SIZE = stoi(argv[++i]);
        else if (a=="--genit" && i+1<argc) GENERATIONS = stoi(argv[++i]);
        else if (a=="--mut" && i+1<argc) MUTATION_RATE = stod(argv[++i]);
        else if (a=="--seed" && i+1<argc) RANDOM_SEED = (unsigned)stoul(argv[++i]);
        else if (a=="--imbalance-mode" && i+1<argc) {
            string m = argv[++i];
            if (m=="avail") GLOBAL_IMB_MODE = IMB_AVAIL;
            else if (m=="used") GLOBAL_IMB_MODE = IMB_USED;
        }
    }

    int hFromCons = 24;
    vector<Device> devices = parseConsumption(consumptionFile, hFromCons);
    HOURS = hFromCons;
    for (int d = 0; d < devices.size(); ++d) {
        cout << "Device " << devices[d].name << ":\n";
        for (int w = 0; w < 7; ++w) {
            // Debug print: show only days with demand or constraints to avoid clutter
            if (devices[d].durationByWeekday[w] > 0 || devices[d].minBlockByWeekday[w] > 0) {
                cout << "  Wk " << w 
                     << " -> Dur: " << devices[d].durationByWeekday[w] 
                     << ", MinBlock: " << devices[d].minBlockByWeekday[w] << "\n";
            }
        }
    }
    if (devices.empty()) { cerr << "No devices parsed. Exiting.\n"; return 1; }
    cout << "Parsed " << devices.size() << " devices. HOURS=" << HOURS << "\n";

    vector<EnergySource> sources = parseGeneration(generationFile, HOURS);
    if (sources.empty()) {
        cerr << "No generation parsed; fallback Grid\n";
        sources.emplace_back("Grid", HOURS);
        sources[0].isGrid = true;
        for (int h=0; h<HOURS; ++h) { sources[0].available[h] = 1e6; sources[0].costPerkWh[h] = 0.20; sources[0].co2PerkWh[h] = 0.7; }
    }
    cout << "Parsed " << sources.size() << " energy sources\n";

    mt19937 rng(RANDOM_SEED);

    // Init population
    vector<Individual> population(POP_SIZE);
    for (int i=0;i<POP_SIZE;++i) {
        population[i].ensureSizes(devices.size(), HOURS, sources.size());
        
        // Heuristic Initialization: For each device and weekday, we will attempt to allocate ON hours in contiguous blocks based on the preferred duration and minimum block size. 
        // This approach is more likely to produce realistic schedules that respect user habits and device constraints, compared to random hour-by-hour initialization.
        for (int w=0; w<7; ++w) {
            for (int d=0; d<(int)devices.size(); ++d) {
                int pref = devices[d].durationByWeekday[w]; // Preferred total ON hours for this device on this weekday
                int minBlk = devices[d].minBlockByWeekday[w]; // Minimum block size for this device on this weekday

                if (minBlk <= 0) minBlk = 1; // Protect case divide by zero
                
                // Initialize population[i].schedule[w][d] based on pref and minBlk
                if (pref <= 0) {
                    for (int h=0; h<HOURS; ++h) population[i].schedule[w][d][h] = 0;
                } else {
                    fill(population[i].schedule[w][d].begin(), population[i].schedule[w][d].end(), 0);

                    int hoursAllocated = 0; // The remain ON hours we still need to allocate for this device on this weekday
                    
                    // Allocate blocks based on preferred duration and minimum block size.
                    while (hoursAllocated < pref) {
                        int currentBlock = min(minBlk, pref - hoursAllocated); // The size of the block we want to allocate in this iteration.
                        vector<int> validStarts; // Store valid start positions for the current block
                        
                        // Find all positions that can fit the current block 
                        for (int h = 0; h <= HOURS - currentBlock; ++h) {
                            bool overlap = false;
                            for (int k = 0; k < currentBlock; ++k) {
                                if (population[i].schedule[w][d][h+k] == 1) overlap = true;
                            }
                            if (!overlap) validStarts.push_back(h);
                        }
                        
                        if (validStarts.empty()) break; // Check if no position.
                        
                        // Randomly select one of the valid start positions and allocate the block there.
                        uniform_int_distribution<> distStart(0, validStarts.size() - 1);
                        int startH = validStarts[distStart(rng)];
                        
                        // Allocate the block at the selected position
                        for (int k = 0; k < currentBlock; ++k) {
                            population[i].schedule[w][d][startH + k] = 1;
                        }
                        hoursAllocated += currentBlock; // Update the allocated hours
                    }
                }
            }
        }
        sanitizeSchedule(population[i], devices);
        evaluateIndividual(population[i], devices, sources);
    }

    ofstream convFile("convergence_log.csv");
    convFile << "Generation,Min_Cost,Min_Discomfort,Min_CO2\n";
    // NSGA-II main loop
    for (int gen=0; gen<GENERATIONS; ++gen) {
        // 1. Non-dominated sorting and rank assignment
        auto norm = normalizeObjectivesMatrix(population);
        auto fronts = fastNonDominatedSort(population, norm);

        for (int r=0; r<(int)fronts.size(); ++r) for (int idx : fronts[r]) population[idx].rank = r;

        // 2. Crowding distance assignment
        for (auto &f : fronts) assignCrowdingDistance(population, f, norm);

        vector<Individual> offspring; // offspring population
        offspring.reserve(POP_SIZE); // reserve space to avoid reallocations
        uniform_real_distribution<> urd(0.0,1.0); // generate a random number generator for real numbers in the range [0, 1]
        uniform_int_distribution<> distPop(0, POP_SIZE-1); // generate a random number generator in the range [0, POP_SIZE-1] for selecting individuals from the population

        // define function for tournament selection based on rank and crowding distance
        auto tournament = [&](const vector<Individual>& popv)->Individual {
            int a = distPop(rng), b = distPop(rng); // randomly select two individuals from the population
            const Individual &A = popv[a], &B = popv[b]; // get the selected individuals

            // Tournament selection: prefer lower rank (better front), and if ranks are equal, prefer higher crowding distance (more diversity)
            if (A.rank < B.rank) return A;
            if (B.rank < A.rank) return B;
            if (A.crowdingDistance > B.crowdingDistance) return A;
            return B;
        };

        // 3. Create offspring through selection, crossover, and mutation
        while ((int)offspring.size() < POP_SIZE) {
            Individual p1 = tournament(population); // select parent 1 using tournament selection
            Individual p2 = tournament(population); // select parent 2 using tournament selection
            auto kids = crossover(p1, p2, rng); // perform crossover to produce two offspring (kids) from the selected parents

            // Apply mutation to each offspring to introduce variability and explore the solution space. The mutation is done in-place on the offspring individuals.
            mutateIndividual(kids.first, devices, rng);
            mutateIndividual(kids.second, devices, rng);
            evaluateIndividual(kids.first, devices, sources);
            evaluateIndividual(kids.second, devices, sources);
            offspring.push_back(kids.first);
            if ((int)offspring.size() < POP_SIZE) offspring.push_back(kids.second);
        }

        // combine & select
        vector<Individual> combined = population;
        combined.insert(combined.end(), offspring.begin(), offspring.end());
        auto normC = normalizeObjectivesMatrix(combined);
        auto combFronts = fastNonDominatedSort(combined, normC);
        for (int r=0; r<(int)combFronts.size(); ++r) for (int idx : combFronts[r]) combined[idx].rank = r;
        for (auto &f : combFronts) assignCrowdingDistance(combined, f, normC);

        vector<Individual> newPop; newPop.reserve(POP_SIZE);
        for (auto &f : combFronts) {
            vector<int> idx = f;
            sort(idx.begin(), idx.end(), [&](int a,int b){ return combined[a].crowdingDistance > combined[b].crowdingDistance; });
            for (int id : idx) {
                if ((int)newPop.size() < POP_SIZE) newPop.push_back(combined[id]);
            }
            if ((int)newPop.size() >= POP_SIZE) break;
        }
        population.swap(newPop);

        if ((gen+1) % 10 == 0) cerr << "Gen " << (gen+1) << "/" << GENERATIONS << " done\n";

        double minCost = numeric_limits<double>::max();
        double minDisc = numeric_limits<double>::max();
        double minCO2 = numeric_limits<double>::max();

        // Find the minimum cost, discomfort, and CO2 values in the current population to track convergence over generations. This will help us understand how the population is evolving in terms of these key objectives.
        for (const auto& ind : population) {
            if (ind.obj_cost < minCost) minCost = ind.obj_cost;
            if (ind.obj_discomfort < minDisc) minDisc = ind.obj_discomfort;
            if (ind.obj_co2 < minCO2) minCO2 = ind.obj_co2;
        }

        // Ghi 1 dòng dữ liệu của thế hệ này vào file CSV
        convFile << gen << "," << minCost << "," << minDisc << "," << minCO2 << "\n";
    }
    convFile.close();

    // final pareto
    auto normFinal = normalizeObjectivesMatrix(population);
    auto finalFronts = fastNonDominatedSort(population, normFinal);
    vector<Individual> pareto;
    if (!finalFronts.empty()) for (int idx : finalFronts[0]) pareto.push_back(population[idx]);
    if (pareto.empty()) { cerr << "Pareto empty\n"; return 0; }

    // sort and export
    sort(pareto.begin(), pareto.end(), [](const Individual &a, const Individual &b){
        if (a.obj_imbalance != b.obj_imbalance) return a.obj_imbalance < b.obj_imbalance;
        if (a.obj_cost != b.obj_cost) return a.obj_cost < b.obj_cost;
        if (a.obj_co2 != b.obj_co2) return a.obj_co2 < b.obj_co2;
        if (a.obj_peakGrid != b.obj_peakGrid) return a.obj_peakGrid < b.obj_peakGrid;
        return a.obj_discomfort < b.obj_discomfort;
    });

    exportParetoJSON(pareto, devices, sources, outPareto);
    cout << "NSGA-II completed. Pareto size = " << pareto.size() << "\n";
    return 0;
}
