#pragma once
#include <string.h>

struct AirlineEntry { char code[4]; char name[26]; };

static const AirlineEntry AIRLINE_TABLE[] = {
  {"AAL", "American Airlines"},
  {"AAR", "Asiana Airlines"},
  {"AAY", "Allegiant Air"},
  {"ABX", "ABX Air"},
  {"ABY", "Air Arabia"},
  {"ACA", "Air Canada"},
  {"AEA", "Air Europa"},
  {"AEE", "Aegean Airlines"},
  {"AFL", "Aeroflot"},
  {"AFR", "Air France"},
  {"AIC", "Air India"},
  {"AMX", "Aeromexico"},
  {"ANA", "All Nippon Airways"},
  {"ANZ", "Air New Zealand"},
  {"ARG", "Aerolineas Argentinas"},
  {"ASA", "Alaska Airlines"},
  {"ASH", "Mesa Airlines"},
  {"ATN", "Air Transport Intl"},
  {"AUA", "Austrian Airlines"},
  {"AVA", "Avianca"},
  {"AWI", "Air Wisconsin"},
  {"AZA", "ITA Airways"},
  {"AZU", "Azul Airlines"},
  {"BAW", "British Airways"},
  {"BEL", "Brussels Airlines"},
  {"BOE", "Boeing"},
  {"BTI", "Air Baltic"},
  {"BTQ", "Boutique Air"},
  {"BWA", "Caribbean Airlines"},
  {"CAL", "China Airlines"},
  {"CCA", "Air China"},
  {"CEB", "Cebu Pacific"},
  {"CES", "China Eastern"},
  {"CFG", "Condor"},
  {"CHH", "Hainan Airlines"},
  {"CJT", "Cargojet"},
  {"CKS", "Kalitta Air"},
  {"CLX", "Cargolux"},
  {"CMP", "Copa Airlines"},
  {"CPA", "Cathay Pacific"},
  {"CSN", "China Southern"},
  {"CXA", "Xiamen Airlines"},
  {"DAH", "Air Algerie"},
  {"DAL", "Delta Air Lines"},
  {"DLH", "Lufthansa"},
  {"EDV", "Endeavor Air"},
  {"EIN", "Aer Lingus"},
  {"EJA", "Executive Jet Aviation"},
  {"EJM", "Executive Jet Mgmt"},
  {"ELY", "El Al"},
  {"ENY", "Envoy Air"},
  {"ETD", "Etihad Airways"},
  {"ETH", "Ethiopian Airlines"},
  {"EWG", "Eurowings"},
  {"EXS", "Jet2"},
  {"EZY", "easyJet"},
  {"FDB", "flydubai"},
  {"FDX", "FedEx"},
  {"FFT", "Frontier Airlines"},
  {"FIN", "Finnair"},
  {"FSI", "FlightSafety Intl"},
  {"GEC", "Lufthansa Cargo"},
  {"GFA", "Gulf Air"},
  {"GJS", "GoJet Airlines"},
  {"GLO", "GOL Airlines"},
  {"GTI", "Atlas Air"},
  {"HAL", "Hawaiian Airlines"},
  {"HVN", "Vietnam Airlines"},
  {"IBE", "Iberia"},
  {"ICE", "Icelandair"},
  {"IGO", "IndiGo"},
  {"IJA", "Intl Jet Aviation Svcs"},
  {"JAL", "Japan Airlines"},
  {"JBU", "JetBlue Airways"},
  {"JIA", "PSA Airlines"},
  {"JRE", "flyExclusive"},
  {"JZA", "Jazz Aviation"},
  {"KAL", "Korean Air"},
  {"KLM", "KLM"},
  {"KQA", "Kenya Airways"},
  {"LAN", "LATAM Airlines"},
  {"LOT", "LOT Polish Airlines"},
  {"LXJ", "Flexjet"},
  {"MAS", "Malaysia Airlines"},
  {"MEA", "Middle East Airlines"},
  {"MSR", "EgyptAir"},
  {"MXY", "Breeze Airways"},
  {"NAX", "Norwegian Air"},
  {"NCB", "Northern Air Cargo"},
  {"NJE", "NetJets Europe"},
  {"NKS", "Spirit Airlines"},
  {"OAL", "Olympic Air"},
  {"PAC", "Polar Air Cargo"},
  {"PAL", "Philippine Airlines"},
  {"PDT", "Piedmont Airlines"},
  {"PGT", "Pegasus Airlines"},
  {"QFA", "Qantas"},
  {"QTR", "Qatar Airways"},
  {"QXE", "Horizon Air"},
  {"RAM", "Royal Air Maroc"},
  {"RBA", "Royal Brunei Airlines"},
  {"RCH", "Air Mobility Command"},
  {"RJA", "Royal Jordanian"},
  {"ROT", "TAROM"},
  {"RPA", "Republic Airways"},
  {"RYR", "Ryanair"},
  {"SAA", "South African Airways"},
  {"SAS", "Scandinavian Airlines"},
  {"SCX", "Sun Country Airlines"},
  {"SEJ", "SpiceJet"},
  {"SIA", "Singapore Airlines"},
  {"SKW", "SkyWest Airlines"},
  {"SOO", "Southern Air"},
  {"SVA", "Saudia"},
  {"SWA", "Southwest Airlines"},
  {"SWR", "SWISS"},
  {"TAM", "LATAM Brasil"},
  {"TAP", "TAP Air Portugal"},
  {"TGW", "Scoot"},
  {"THA", "Thai Airways"},
  {"THY", "Turkish Airlines"},
  {"TOM", "TUI Airways"},
  {"TRA", "Transavia"},
  {"TSC", "Air Transat"},
  {"UAE", "Emirates"},
  {"UAL", "United Airlines"},
  {"UPS", "UPS Airlines"},
  {"UZB", "Uzbekistan Airways"},
  {"VIR", "Virgin Atlantic"},
  {"VIV", "VivaAerobus"},
  {"VJC", "VietJet Air"},
  {"VJT", "VistaJet"},
  {"VLG", "Vueling"},
  {"VOI", "Volaris"},
  {"VOZ", "Virgin Australia"},
  {"WJA", "WestJet"},
  {"WUP", "Wheels Up"},
  {"WZZ", "Wizz Air"},
};
static const int AIRLINE_TABLE_SIZE =
  (int)(sizeof(AIRLINE_TABLE) / sizeof(AIRLINE_TABLE[0]));

// Returns the airline name for a callsign's ICAO prefix (e.g. "AAL123" -> "American Airlines"),
// or nullptr if the callsign isn't a scheduled airline flight or the code isn't in the table.
static const char *airlineLookup(const char *callsign) {
  char prefix[4] = {0, 0, 0, 0};
  int  plen = 0;
  while (plen < 3 && callsign[plen] >= 'A' && callsign[plen] <= 'Z') {
    prefix[plen] = callsign[plen];
    plen++;
  }
  if (plen < 2) return nullptr;
  bool hasNum = false;
  for (int j = plen; callsign[j]; j++)
    if (callsign[j] >= '0' && callsign[j] <= '9') { hasNum = true; break; }
  if (!hasNum) return nullptr;

  int lo = 0, hi = AIRLINE_TABLE_SIZE - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    int cmp = strcmp(prefix, AIRLINE_TABLE[mid].code);
    if (cmp == 0) return AIRLINE_TABLE[mid].name;
    if (cmp < 0)  hi = mid - 1;
    else          lo = mid + 1;
  }
  return nullptr;
}
