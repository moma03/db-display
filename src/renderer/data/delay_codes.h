#ifndef DELAY_CODES_H
#define DELAY_CODES_H

// delay_codes.h
// Maps Deutsche Bahn RIS delay/quality reason codes to human-readable
// German strings.  Generated from the official RIS code table
// (2025_42_Uebersicht-RIS-Kundengruende-und-Qualitaetsabweichungen).
//
// Code types:
//   R = Kundengrund  (customer-facing delay reason)
//   Q = Qualitätsabweichung (quality deviation)

#include <string>
#include <unordered_map>

// Returns the reason text for a given numeric code, or "" if unknown.
inline const std::string &DelayReasonText(int code) {
    // Lazy-initialised static map
    static const std::unordered_map<int, std::string> table = {
        // --- R: Delay reasons (Kundengründe) ---
        { 0, "Keine Verspätungsbegründung"},
        { 1, "Nähere Informationen in Kürze"},
        { 2, "Polizeieinsatz"},
        { 3, "Feuerwehreinsatz auf der Strecke"},
        // 4: unused
        { 5, "Ärztliche Versorgung eines Fahrgastes"},
        { 6, "Unbefugtes Ziehen der Notbremse"},
        { 7, "Unbefugte Personen auf der Strecke"},
        { 8, "Notarzteinsatz auf der Strecke"},
        { 9, "Streikauswirkungen"},
        {10, "Tiere auf der Strecke"},
        {11, "Unwetter"},
        {12, "Warten auf ein verspätetes Schiff"},
        {13, "Pass- und Zollkontrolle"},
        {14, "Technischer Defekt am Bahnhof"},
        {15, "Beeinträchtigung durch Vandalismus"},
        {16, "Entschärfung einer Fliegerbombe"},
        {17, "Beschädigung einer Brücke"},
        {18, "Umgestürzter Baum auf der Strecke"},
        {19, "Unfall an einem Bahnübergang"},
        // 20: unused
        {21, "Warten auf Anschlussreisende"},
        {22, "Witterungsbedingte Beeinträchtigungen"},
        // 23: deleted
        {24, "Verspätung im Ausland"},
        {25, "Bereitstellung weiterer Wagen"},
        {26, "Abhängen von Wagen"},
        {27, "Technische Störung am Bus"},
        {28, "Gegenstände auf der Strecke"},
        {29, "Ersatzverkehr mit Bus ist eingerichtet"},
        {30, "Personalausfall im Stellwerk"},
        {31, "Bauarbeiten"},
        {32, "Längere Haltezeit am Bahnhof"},
        {33, "Reparatur an der Oberleitung"},
        {34, "Reparatur an einem Signal"},
        {35, "Streckensperrung"},
        {36, "Technische Störung am Zug"},
        {37, "Kurzfristiger Fahrzeugausfall"},
        {38, "Reparatur an der Strecke"},
        {39, "Stau / Hohes Verkehrsaufkommen"},
        {40, "Defektes Stellwerk"},
        {41, "Technischer Defekt an einem Bahnübergang"},
        {42, "Vorübergehend verminderte Geschwindigkeit auf der Strecke"},
        {43, "Verspätung eines vorausfahrenden Zuges"},
        {44, "Warten auf einen entgegenkommenden Zug"},
        {45, "Vorfahrt eines anderen Zuges"},
        // 46: unused
        {47, "Verspätete Bereitstellung des Zuges"},
        {48, "Verspätung aus vorheriger Fahrt"},
        {49, "Kurzfristiger Personalausfall"},
        {50, "Kurzfristige Erkrankung von Personal"},
        {51, "Verspätetes Personal aus vorheriger Fahrt"},
        {52, "Streik"},
        {53, "Unwetterauswirkungen"},
        {54, "Verfügbarkeit der Gleise derzeit eingeschränkt"},
        {55, "Technischer Defekt an einem anderen Zug"},
        {56, "Laden der Antriebsbatterie"},
        {57, "Zusätzlicher Halt zum Ein- und Ausstieg"},
        {58, "Umleitung des Zuges"},
        {59, "Schnee und Eis"},
        {60, "Witterungsbedingt verminderte Geschwindigkeit"},
        {61, "Defekte Tür"},
        {62, "Behobener technischer Defekt am Zug"},
        {63, "Technische Untersuchung am Zug"},
        {64, "Reparatur an einer Weiche"},
        {65, "Erdrutsch"},
        {66, "Hochwasser"},
        {67, "Behördliche Maßnahme"},
        {68, "Hohes Fahrgastaufkommen verlängert Ein- und Ausstieg"},
        {69, "Zug verkehrt mit verminderter Geschwindigkeit"},
        {99, "Verzögerungen im Betriebsablauf"},

        // --- Q: Quality deviations (Qualitätsabweichungen) ---
        {70, "WLAN nicht verfügbar"},
        {71, "Eingeschränktes WLAN"},
        {72, "Info-/Entertainment nicht verfügbar"},
        {73, "Heute: Mehrzweckabteil vorne"},
        {74, "Heute: Mehrzweckabteil hinten"},
        {75, "Heute: 1. Klasse vorne"},
        {76, "Heute: 1. Klasse hinten"},
        {77, "1. Klasse fehlt"},
        {78, "Ersatzverkehr mit Bus ist eingerichtet"},
        {79, "Mehrzweckabteil fehlt"},
        {80, "Andere Reihenfolge der Wagen"},
        // 81: deleted
        {82, "Mehrere Wagen fehlen"},
        {83, "Heute ohne fahrzeuggebundene Einstiegshilfe"},
        {84, "Zug verkehrt richtig gereiht"},
        {85, "Ein Wagen fehlt"},
        {86, "Gesamter Zug ohne Reservierung"},
        {87, "Einzelne Wagen ohne Reservierung"},
        {88, "Keine Qualitätsmängel"},
        {89, "Reservierungen sind wieder vorhanden"},
        {90, "Kein gastronomisches Angebot"},
        {91, "Fahrradmitnahme nicht möglich"},
        {92, "Fahrradmitnahme kann nicht garantiert werden"},
        {93, "Behindertengerechte Einrichtung fehlt"},
        {94, "Ersatzbewirtschaftung"},
        {95, "Universaltoilette fehlt"},
        {96, "Zustieg kann nicht garantiert werden"},
        {97, "Hohe Auslastung"},
        {98, "Sonstige Qualitätsmängel"},
    };

    static const std::string empty;
    auto it = table.find(code);
    return (it != table.end()) ? it->second : empty;
}

#endif // DELAY_CODES_H
