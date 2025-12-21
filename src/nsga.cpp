// NSGA-II for Energy Flexibility 
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
    // derived
    vector<int> durationByWeekday;      // count ON hours inferred per weekday
    vector<int> minBlockByWeekday;      // minimal consecutive ON block (in hours) per weekday (0=>no demand)
    int maxSwitches = 6;

    Device(const string &n = "", int hours = 24) : name(n) {
        weekdayHour.assign(7, vector<double>(hours, 0.0));
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
    ss >> get_time(&t, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        ss.clear();
        ss.str(ts);
        ss >> get_time(&t, "%Y-%m-%d %H:%M");
        if (ss.fail()) return false;
    }
    // normalize
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
    vector<string> out; string cur; stringstream ss(s);
    while (getline(ss, cur, delim)) out.push_back(cur);
    return out;
}

// ---------------- parseConsumption (3-tier) ----------------
//
// Input: CSV with header: Timestamp;Entity Name;SP01;SP02;...
// Tier1: for each device, for each weekday+hour, pick MAX observed >0
// Tier2: if a particular weekday has NO observation at that hour but there exist days with same weekday in dataset:
//        we will infer hour-power only when considering moving schedule TO that hour: we compute
//        - modal minimal consecutive ON block (majority) and number of ON blocks per day (majority) for that weekday
//        - average power over observed ON hours in that weekday across dataset for that device
// Tier3: if no observations at all for that device on that weekday, device has NO demand for that weekday (all zeros)
// Note: function returns vector<Device> and sets outHours (HOURS)
vector<Device> parseConsumption(const string &filename, int &outHours) {
    ifstream ifs(filename);
    if (!ifs.is_open()) {
        cerr << "Cannot open consumption file: " << filename << "\n";
        return {};
    }
    string header;
    if (!getline(ifs, header)) { cerr << "Empty consumption file\n"; return {}; }
    // detect delimiter ; or ,
    char delim = (header.find(';') != string::npos) ? ';' : ',';
    vector<string> cols = splitDelim(header, delim);
    int deviceCount = max(0, (int)cols.size() - 2);
    if (deviceCount <= 0) {
        cerr << "No device columns found in header\n";
        return {};
    }

    vector<vector<double>> rawPerDevice(deviceCount);
    vector<pair<int,int>> timeInfo; // (weekday, hour) per row
    string line;
    while (getline(ifs, line)) {
        if (line.empty()) continue;
        // normalize decimals in cell parsing only (we will replace ,->. inside each cell later)
        vector<string> cells = splitDelim(line, delim);
        if ((int)cells.size() < 2+deviceCount) {
            // pad
            cells.resize(2+deviceCount);
        }
        int wd=0,h=0;
        if (!parseTimestampToWeekHour(cells[0], wd, h)) continue;
        timeInfo.emplace_back(wd,h);
        for (int p=0;p<deviceCount;++p) {
            string cell = (2+p < (int)cells.size()) ? cells[2+p] : string();
            for (char &c: cell) if (c==',') c='.';
            double v = 0.0;
            if (!cell.empty()) {
                try { v = stod(cell); } catch(...) { v = 0.0; }
            }
            rawPerDevice[p].push_back(v);
        }
    }

    outHours = 24;
    HOURS = outHours;
    vector<Device> devices;
    devices.reserve(deviceCount);
    for (int p=0;p<deviceCount;++p) devices.emplace_back("D"+to_string(p+1), HOURS);

    int R = timeInfo.size();
    // Bucket observed positives by device, weekday, hour
    for (int p=0;p<deviceCount;++p) {
        // bucket[w][h] -> vector values
        vector<vector<vector<double>>> bucket(7, vector<vector<double>>(HOURS));
        for (int r=0;r<R;++r) {
            int wd = timeInfo[r].first;
            int hr = timeInfo[r].second;
            double v = rawPerDevice[p][r];
            if (v > 0.0) bucket[wd][hr].push_back(v);
        }
        // Tier1: pick MAX per weekday+hour
        for (int w=0; w<7; ++w) {
            int nonzero=0;
            for (int h=0; h<HOURS; ++h) {
                if (!bucket[w][h].empty()) {
                    double mx = *max_element(bucket[w][h].begin(), bucket[w][h].end());
                    devices[p].weekdayHour[w][h] = mx;
                    ++nonzero;
                } else {
                    devices[p].weekdayHour[w][h] = 0.0;
                }
            }
            // compute durationByWeekday if nonzero
            if (nonzero>0) {
                int dur=0; for (int h=0; h<HOURS; ++h) if (devices[p].weekdayHour[w][h] > 0.0) ++dur;
                devices[p].durationByWeekday[w] = dur;
            } else devices[p].durationByWeekday[w] = 0;
        }

        // Tier2 prep: analyze days that have any positive for each weekday
        // We'll need per-day sequences for counting minimal consecutive ON blocks and number of blocks
        // Reconstruct per-row data per weekday -> per-day sequences
        // Build map weekday -> list of day sequences (vector<int> length HOURS with 0/1)
        vector<vector<vector<int>>> daySeqs(7); // for each weekday, list of day binary sequences
        for (int r=0;r<R;++r) {
            int wd = timeInfo[r].first;
            int hr = timeInfo[r].second;
            // we assume rows come ordered by datetime but may span many days; we collect per-day by grouping consecutive rows with same date+weekday
            // Simpler approach: build a map from (date string) to day index; but we didn't store date string. So we will reconstruct by grouping by consecutive occurrences:
        }
        // Simpler robust approach: re-read file to build per-day sequences with full date string
        ifstream ifs2(filename);
        string hdr2; getline(ifs2, hdr2);
        vector<pair<string, vector<double>>> dayRecords; // dateKey -> flattened vector of device values per hour
        // We will gather rows grouped by date (YYYY-MM-DD)
        map<string, vector<pair<int, vector<double>>>> grouped; // date -> list of (hour, deviceValues)
        while (getline(ifs2, line)) {
            if (line.empty()) continue;
            vector<string> cells = splitDelim(line, delim);
            if (cells.empty()) continue;
            string ts = cells[0];
            // extract date part YYYY-MM-DD
            string datePart;
            if (ts.size() >= 10) datePart = ts.substr(0,10);
            int wd, hr;
            if (!parseTimestampToWeekHour(ts, wd, hr)) continue;
            vector<double> vals(deviceCount, 0.0);
            for (int p2=0;p2<deviceCount;++p2) {
                string cell = (2+p2 < (int)cells.size()) ? cells[2+p2] : string();
                for (char &c: cell) if (c==',') c='.';
                if (!cell.empty()) {
                    try { vals[p2] = stod(cell); } catch(...) { vals[p2]=0.0; }
                }
            }
            grouped[datePart].push_back({hr, vals});
        }
        // Now for each date, build full-day vector per device
        for (auto &kv : grouped) {
            auto &entries = kv.second;
            // sort by hour
            sort(entries.begin(), entries.end(), [](auto &a, auto &b){ return a.first < b.first; });
            // derive weekday from first row time
            if (entries.empty()) continue;
            // find weekday from any row: reconstruct timestamp? we don't have full ts here; but we can call parseTimestampToWeekHour on date + " 00:00:00"
            string dt = kv.first + " 00:00:00";
            int dd_wd, dd_hr;
            if (!parseTimestampToWeekHour(dt, dd_wd, dd_hr)) continue;
            vector<int> bin(HOURS, 0);
            vector<double> powAvg(HOURS, 0.0);
            vector<int> cnt(HOURS,0);
            for (auto &pr : entries) {
                int hr = pr.first;
                if (hr < 0 || hr >= HOURS) continue;
                double v = pr.second[p]; // device p's value
                if (v > 0.0) { bin[hr] = 1; powAvg[hr] += v; cnt[hr]++; }
            }
            // store bin sequence for that weekday
            daySeqs[dd_wd].push_back(bin);
        }

        // For each weekday, if daySeqs[w] non-empty -> compute minimal ON block and modal number of blocks per day and average power over ON hours
        for (int w=0; w<7; ++w) {
            if (daySeqs[w].empty()) {
                devices[p].minBlockByWeekday[w] = 0;
                continue;
            }
            // compute minimal block lengths found in sequences (we'll collect all run-lengths)
            vector<int> blockLens; // all minimal run lengths per day (we'll push per-day minimal positive run >0)
            vector<int> blocksPerDay;
            vector<double> onPowers; // collect all observed positive powers for this weekday across all days/hours (for avg)
            for (auto &seq : daySeqs[w]) {
                int n = seq.size();
                int i=0;
                vector<int> runs;
                while (i<n) {
                    while (i<n && seq[i]==0) ++i;
                    if (i>=n) break;
                    int j=i;
                    while (j<n && seq[j]==1) ++j;
                    int len = j-i;
                    runs.push_back(len);
                    i=j;
                }
                if (!runs.empty()) {
                    int minrun = *min_element(runs.begin(), runs.end());
                    blockLens.push_back(minrun);
                    blocksPerDay.push_back((int)runs.size());
                }
            }
            if (!blockLens.empty()) {
                // mode of blockLens
                map<int,int> freq;
                for (int v : blockLens) freq[v]++;
                int best = 0; int bestc = 0;
                for (auto &kv2 : freq) if (kv2.second > bestc) { bestc = kv2.second; best = kv2.first; }
                devices[p].minBlockByWeekday[w] = best;
            } else devices[p].minBlockByWeekday[w] = 0;

            // collect on powers
            // iterate all daySeqs[w] again but read real power from devices[p].weekdayHour (tier1) where >0
            for (auto &seq : daySeqs[w]) {
                for (int h=0; h<HOURS; ++h) {
                    if (seq[h] == 1) {
                        double val = devices[p].weekdayHour[w][h];
                        if (val > 0.0) onPowers.push_back(val);
                    }
                }
            }
            // If onPowers empty, we may still leave weekdayHour as is (Tier1 had none)
            if (!onPowers.empty()) {
                double sum=accumulate(onPowers.begin(), onPowers.end(), 0.0);
                double avg = sum / onPowers.size();
                // store a recommended avg for missing hours via a temporary trick: we DO NOT overwrite all 24h.
                // Instead we'll leave weekdayHour as-is (Tier1) and treat avg when trying to place a device at an hour lacking data.
                // To keep info accessible, we temporarily store avg in hours that are zero only if minBlockByWeekday>0? NO — we must not write globally.
                // We'll store it in a map external to devices if needed. For simplicity, leave devices[p].weekdayHour[w][h] as currently (Tier1).
            }
        }
    } // end per-device loop

    // Important: Tier2 and Tier3 behavior will be used at scheduling time / evaluation:
    // - If a schedule requests turning device on at weekday w hour h but devices[p].weekdayHour[w][h]==0:
    //    * If daySeqs[w] existed (we computed minBlockByWeekday[w]>0) -> treat this hour candidate as allowed and use avg power computed from observed ON hours (we didn't persist avg, but we can recompute quickly if needed)
    //    * Else (minBlock==0) -> device has no demand for this weekday: we should force schedule to 0 for that weekday.

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
    int tokens = rows[0].size();
    int triples = max(1, (tokens - 1) / 3);
    vector<string> names;
    for (int t=0;t<triples;++t) {
        if (t==0) names.push_back("Grid");
        else names.push_back(string(1, char('A' + (t-1))));
    }
    vector<EnergySource> sources;
    for (int t=0;t<triples;++t) sources.emplace_back(names[t], expectedHours);
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
    // detect grid by name if header provided? We used default names, allow user to rename later.
    if (!sources.empty()) sources[0].isGrid = true;
    return sources;
}

// ---------------- NSGA utilities ----------------
vector<vector<double>> normalizeObjectivesMatrix(const vector<Individual>& pop) {
    int N = pop.size();
    int m = 5;
    vector<double> minv(m, numeric_limits<double>::infinity()), maxv(m, -numeric_limits<double>::infinity());
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
    vector<vector<double>> out(N, vector<double>(m,0.0));
    for (int i=0;i<N;++i) {
        double vals[5] = { pop[i].obj_imbalance, pop[i].obj_cost, pop[i].obj_co2, pop[i].obj_peakGrid, pop[i].obj_discomfort };
        for (int j=0;j<m;++j) {
            double denom = maxv[j] - minv[j];
            if (denom < 1e-12) denom = 1.0;
            out[i][j] = (vals[j] - minv[j]) / denom;
        }
    }
    return out;
}

bool dominates(const vector<double>& a, const vector<double>& b) {
    bool strictly=false;
    for (size_t i=0;i<a.size();++i) {
        if (a[i] > b[i] + 1e-12) return false;
        if (a[i] < b[i] - 1e-12) strictly = true;
    }
    return strictly;
}

vector<vector<int>> fastNonDominatedSort(const vector<Individual>& pop, const vector<vector<double>>& norm) {
    int N = pop.size();
    vector<int> domCount(N,0);
    vector<vector<int>> dominated(N);
    vector<vector<int>> fronts;
    fronts.emplace_back();
    for (int p=0;p<N;++p) {
        for (int q=0;q<N;++q) {
            if (p==q) continue;
            if (dominates(norm[p], norm[q])) dominated[p].push_back(q);
            else if (dominates(norm[q], norm[p])) domCount[p]++;
        }
        if (domCount[p]==0) fronts[0].push_back(p);
    }
    int idx=0;
    while (idx < (int)fronts.size()) {
        vector<int> next;
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
    int l = front.size();
    if (l==0) return;
    for (int idx : front) pop[idx].crowdingDistance = 0.0;
    int m = norm[0].size();
    for (int obj=0; obj<m; ++obj) {
        vector<int> tmp = front;
        sort(tmp.begin(), tmp.end(), [&](int a,int b){ return norm[a][obj] < norm[b][obj]; });
        pop[tmp.front()].crowdingDistance = numeric_limits<double>::infinity();
        pop[tmp.back()].crowdingDistance  = numeric_limits<double>::infinity();
        double fmin = norm[tmp.front()][obj];
        double fmax = norm[tmp.back()][obj];
        if (fabs(fmax - fmin) < 1e-12) continue;
        for (int i=1;i<(int)tmp.size()-1;++i) {
            pop[tmp[i]].crowdingDistance += (norm[tmp[i+1]][obj] - norm[tmp[i-1]][obj]) / (fmax - fmin);
        }
    }
}

// crossover: per-weekday, per-device single point across hours; produce children by swapping suffix after point for every weekday and device
pair<Individual,Individual> crossover(const Individual &p1, const Individual &p2, mt19937 &rng) {
    Individual c1 = p1, c2 = p2;
    uniform_int_distribution<> dist(0, max(0,HOURS-1));
    int point = dist(rng);
    int nDevices = p1.schedule[0].size();
    for (int w=0; w<7; ++w) {
        for (int d=0; d<nDevices; ++d) {
            for (int h=point; h<HOURS; ++h) swap(c1.schedule[w][d][h], c2.schedule[w][d][h]);
        }
    }
    return {c1,c2};
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

// mutate: bitflip but respect devices' possible ON hours (avoid turning ON hours that are forbidden)
void mutateIndividual(Individual &ind, const vector<Device>& devices, mt19937 &rng) {
    uniform_real_distribution<> urd(0.0,1.0);
    int nD = devices.size();
    for (int w=0; w<7; ++w) {
        for (int d=0; d<nD; ++d) {
            for (int h=0; h<HOURS; ++h) {
                if (urd(rng) < MUTATION_RATE) {
                    ind.schedule[w][d][h] ^= 1;
                }
            }
        }
    }
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
        // compute load: use Tier1 value if >0, else if Tier2 (minBlockByWeekday>0) infer average from observed ON hours in that weekday, otherwise treat as zero and force schedule=0
        for (int d=0; d<nD; ++d) {
            for (int h=0; h<HOURS; ++h) {
                if (ind.schedule[w][d][h]) {
                    double power = devices[d].weekdayHour[w][h];
                    if (power > 0.0) {
                        load[h] += power;
                    } else {
                        // Tier2 allowed?
                        if (devices[d].minBlockByWeekday[w] > 0) {
                            // estimate avg power for this device & weekday by averaging nonzero Tier1 values of that weekday
                            double sum=0; int cnt=0;
                            for (int hh=0; hh<HOURS; ++hh) {
                                if (devices[d].weekdayHour[w][hh] > 0.0) { sum += devices[d].weekdayHour[w][hh]; ++cnt; }
                            }
                            double est = (cnt>0) ? (sum / cnt) : 0.0;
                            if (est > 0.0) load[h] += est;
                            else {
                                // fallback: zero -> force off
                                ind.schedule[w][d][h] = 0;
                            }
                        } else {
                            // Tier3 -> no demand -> force off
                            ind.schedule[w][d][h] = 0;
                        }
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
            vector<int> pref(HOURS,0);
            int prefCount = 0;
            for (int h=0; h<HOURS; ++h) if (devices[d].weekdayHour[w][h] > 0.0) { pref[h]=1; ++prefCount; }
            int onCount=0;
            for (int h=0; h<HOURS; ++h) if (ind.schedule[w][d][h]) ++onCount;
            double missingFraction = 0.0;
            if (prefCount>0) {
                int miss=0;
                for (int h=0; h<HOURS; ++h) if (pref[h]==1 && ind.schedule[w][d][h]==0) ++miss;
                missingFraction = double(miss)/double(prefCount);
            }
            int switches=0;
            for (int h=1; h<HOURS; ++h) if (ind.schedule[w][d][h] != ind.schedule[w][d][h-1]) ++switches;
            double excessSwitchFraction = 0.0;
            if (devices[d].maxSwitches > 0) {
                int ex = max(0, switches - devices[d].maxSwitches);
                excessSwitchFraction = min(1.0, double(ex) / double(devices[d].maxSwitches));
            }
            int hamming=0;
            for (int h=0; h<HOURS; ++h) {
                int p = (devices[d].weekdayHour[w][h] > 0.0) ? 1 : 0;
                if (p != ind.schedule[w][d][h]) ++hamming;
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
        // randomize by respecting durationByWeekday: for each weekday, for each device choose ON hours among observed ON hours if any
        for (int w=0; w<7; ++w) {
            for (int d=0; d<(int)devices.size(); ++d) {
                int pref = devices[d].durationByWeekday[w];
                if (pref <= 0) {
                    // keep zeros (no demand)
                    for (int h=0; h<HOURS; ++h) population[i].schedule[w][d][h] = 0;
                } else {
                    // prefer hours with tier1 data >0
                    vector<int> cand;
                    for (int h=0; h<HOURS; ++h) if (devices[d].weekdayHour[w][h] > 0.0) cand.push_back(h);
                    if (cand.empty()) {
                        // fallback to any hour
                        for (int h=0; h<HOURS; ++h) cand.push_back(h);
                    }
                    shuffle(cand.begin(), cand.end(), rng);
                    fill(population[i].schedule[w][d].begin(), population[i].schedule[w][d].end(), 0);
                    for (int k=0; k<min((int)cand.size(), pref); ++k) population[i].schedule[w][d][cand[k]] = 1;
                }
            }
        }
        sanitizeSchedule(population[i], devices);
        evaluateIndividual(population[i], devices, sources);
    }

    // NSGA-II main loop
    for (int gen=0; gen<GENERATIONS; ++gen) {
        auto norm = normalizeObjectivesMatrix(population);
        auto fronts = fastNonDominatedSort(population, norm);
        for (int r=0; r<(int)fronts.size(); ++r) for (int idx : fronts[r]) population[idx].rank = r;
        for (auto &f : fronts) assignCrowdingDistance(population, f, norm);

        // offspring
        vector<Individual> offspring; offspring.reserve(POP_SIZE);
        uniform_real_distribution<> urd(0.0,1.0);
        uniform_int_distribution<> distPop(0, POP_SIZE-1);
        auto tournament = [&](const vector<Individual>& popv)->Individual {
            int a = distPop(rng), b = distPop(rng);
            const Individual &A = popv[a], &B = popv[b];
            if (A.rank < B.rank) return A;
            if (B.rank < A.rank) return B;
            if (A.crowdingDistance > B.crowdingDistance) return A;
            return B;
        };
        while ((int)offspring.size() < POP_SIZE) {
            Individual p1 = tournament(population);
            Individual p2 = tournament(population);
            auto kids = crossover(p1, p2, rng);
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
    }

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
