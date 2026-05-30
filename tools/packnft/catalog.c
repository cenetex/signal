#include "catalog.h"
#include "../../shared/sha256.h"

static void sha256_hex(const uint8_t hash[32], char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2] = hx[hash[i]>>4]; out[i*2+1] = hx[hash[i]&15]; }
    out[64] = 0;
}

const char FIRST_BELL_SET_NAME[]  = "Ruby High: First Bell";
const char FIRST_BELL_SET_CODE[]  = "FB";
const char FIRST_BELL_SET_FAMILY[] = "Ruby High";
const char FIRST_BELL_PACK_SHAPE[CARDS_PER_PACK][24] = {
    "teacher", "student", "student", "student", "utility-or-special"
};

const card_profile_t FIRST_BELL_CATALOG[FIRST_BELL_PROFILE_COUNT] = {
    /* Teachers (common) — set numbers 1-3 */
    { "ruby",            "Ruby",             "teacher", "common",      "Homeroom Card",
      "Ruby stamped this one before the late bell could object.",
      "#d22a2a", "teachers", "0% 0%",   NULL,  1, "ruby",            "Ruby · Homeroom",  "Homeroom",           "ruby", 1, NULL, NULL },
    { "sally-science",   "Sally Science",    "teacher", "common",      "Lab Sink Shortcut",
      "Good for one escape from sloppy variables.",
      "#35b978", "teachers", "50% 0%",  NULL,  2, "sally-science",   "Sally · Lab Sink", "Science",            "sally", 1, NULL, NULL },
    { "professor-edward","Professor Edward", "teacher", "common",      "Library Corridor Pass",
      "Please return before the footnotes start breeding.",
      "#b79243", "teachers", "100% 0%", NULL,  3, "professor-edward","Edward · Corridor", "Literature",         "edward", 1, NULL, NULL },

    /* Students (common) — set numbers 4-6 */
    { "lyra", "Lyra", "student", "common", "Color-Coded Spare",
      "Lyra made three backups and labeled this one urgent.",
      "#ff6f91", "students", "0% 0%",   "Lyra slipped this one into the stack.", 4, "lyra", "Lyra · Color-Coded",   "General Studies", "lyra", 1, NULL, NULL },
    { "sami", "Sami", "student", "common", "Side Door Whatever",
      "Sami says it works if you look bored enough.",
      "#36c2cc", "students", "50% 0%",  "Sami slipped this one into the stack.", 5, "sami", "Sami · Side Door",      "General Studies", "sami", 1, NULL, NULL },
    { "ravi", "Ravi", "student", "common", "Field Trip Fact Slip",
      "Ravi has a tangent ready for the entire walk.",
      "#ffb05a", "students", "100% 0%", "Ravi slipped this one into the stack.", 6, "ravi", "Ravi · Field Trip",      "General Studies", "ravi", 1, NULL, NULL },

    /* Students (rare) — set numbers 7-9 */
    { "indra","Indra","student", "rare",   "Quiet Perfect Exit",
      "Indra noticed the pattern and left before anyone clapped.",
      "#a06bff", "students", "0% 100%",  "Indra noticed the pattern before anyone clapped.", 7, "indra","Indra · Quiet Exit",    "General Studies", "indra", 1, NULL, NULL },
    { "mika", "Mika", "student", "rare",   "Locker Room Shortcut",
      "Mika says you are absolutely cleared for this.",
      "#52c673", "students", "50% 100%", NULL, 8, "mika", "Mika · Locker Room",    "General Studies", "mika", 1, NULL, NULL },
    { "noor", "Noor", "student", "rare",   "Deadpan Detour",
      "Noor called it a plot hole and walked through it.",
      "#ec4f9e", "students", "100% 100%", NULL, 9, "noor", "Noor · Deadpan Detour",    "General Studies", "noor", 1, NULL, NULL },

    /* Super-rare teachers — set numbers 10-11 */
    { "eliza","Eliza","teacher", "super-rare","Systems Lab Override",
      "Eliza makes the system legible, then makes it sing.",
      "#62d3c2", "specials", "50% 0%",  "Make the system legible, then make it sing.", 10, "eliza","Eliza · Systems Lab", "Systems", "eliza", 1, NULL, NULL },
    { "rati", "Rati", "teacher", "super-rare","Signal Studies Pass",
      "Hold the signal. Build the world.",
      "#f0a12a", "specials", "100% 0%", NULL, 11, "rati", "Rati · Signal Studies", "Signal Studies", "rati", 1, NULL, NULL },

    /* Ultra-rare special — set number 12 */
    { "captain-null","Captain Null","special","ultra-rare","Page 10 Shadow Pass",
      "Find page 10 and the hallway forgets your name.",
      "#111111", "specials", "0% 0%",   NULL, 12, "captain-null","Captain Null · Shadow Pass","???", "captain-null", 1, NULL, NULL },

    /* Items (common) — set numbers 13-15 */
    { "item-hall-pass",    "Hall Pass",    "item", "common", "Front Office Reset",
      "Sometimes the smartest move is stepping out and coming back better.",
      "#f14a4a", "items", "0% 0%",   NULL, 13, "item-hall-pass",    "Hall Pass · Front Office","General", "item-hall-pass", 1, NULL, NULL },
    { "item-merit-star",   "Merit Star",   "item", "common", "Gold Star Slip",
      "One day of perfect answers, pressed into paper.",
      "#ffd700", "items", "0% 0%",   NULL, 14, "item-merit-star",   "Merit Star · Gold Slip", "General", "item-merit-star", 1, NULL, NULL },
    { "item-locker-note",  "Locker Note",  "item", "common", "Locker 204 Note",
      "Someone left this wedged in the vent. It still smells like chalk.",
      "#a0d2db", "items", "0% 0%",   NULL, 15, "item-locker-note",  "Locker Note · 204",      "General", "item-locker-note", 1, NULL, NULL },

    /* Students continued (rare/common) — set numbers 16-24 (live profiles fill to 24) */
    { "lyra","Lyra","student","rare","Study Hall Vigil",
      "Lyra stayed after the bell and caught a second wind.",
      "#ff6f91","students","0% 50%",NULL,16,"lyra-v2","Lyra · Study Hall","General Studies","lyra-v2",1,"lyra","alt-art"},
    { "sami","Sami","student","rare","Detention Slip Reversal",
      "Sami turned detention into a free period and nobody noticed.",
      "#36c2cc","students","50% 50%",NULL,17,"sami-v2","Sami · Detention Slip","General Studies","sami-v2",1,"sami","alt-art"},
    { "ravi","Ravi","student","rare","Unscheduled Fire Drill",
      "Ravi asked a question so interesting the alarm went off.",
      "#ffb05a","students","100% 50%",NULL,18,"ravi-v2","Ravi · Fire Drill","General Studies","ravi-v2",1,"ravi","alt-art"},
    { "indra","Indra","student","common","Perfect Attendance Ghost",
      "Indra was here. The attendance sheet proves it. Nobody saw her.",
      "#a06bff","students","0% 50%",NULL,19,"indra-v2","Indra · Attendance","General Studies","indra-v2",1,"indra","alt-art"},
    { "mika","Mika","student","common","Gym Class Record",
      "Mika broke a record that wasn't even on the books yet.",
      "#52c673","students","50% 50%",NULL,20,"mika-v2","Mika · Gym Record","General Studies","mika-v2",1,"mika","alt-art"},
    { "noor","Noor","student","common","Yearbook Quote Heist",
      "Noor swapped every senior quote with a single comma. It was better.",
      "#ec4f9e","students","100% 50%",NULL,21,"noor-v2","Noor · Quote Heist","General Studies","noor-v2",1,"noor","alt-art"},

    /* Locations (common) — set numbers 22-24 */
    { "location-classroom","Room 101", "location","common","Homeroom 101",
      "The chalkboard still has yesterday's equation half-erased in the corner.",
      "#8b7355","locations","0% 0%",NULL,22,"location-classroom","Room 101 · Homeroom","Location","classroom",1,NULL,NULL},
    { "location-library",  "Library",  "location","common","Third Floor Stacks",
      "The lights flicker on a timer that hasn't been right since 1998.",
      "#6b4423","locations","0% 0%",NULL,23,"location-library",  "Library · Stacks",      "Location","library",1,NULL,NULL},
    { "location-cafeteria", "Cafeteria","location","common","Table Seven",
      "This table has seen three food fights and one promposal. It remembers.",
      "#c4a35a","locations","0% 0%",NULL,24,"location-cafeteria","Cafeteria · Table 7",   "Location","cafeteria",1,NULL,NULL},

    /* Expansion profiles (25-36) — mintable but not yet active */
    { "lyra","Lyra","student","rare","Fire Drill Rehearsal",
      "Lyra already knows where to stand.",
      "#ff6f91","students","0% 75%",NULL,25,"lyra-x1","Lyra · Fire Drill","General Studies","lyra-x1",0,"lyra","expansion"},
    { "sami","Sami","student","rare","Substitute Teacher Decoy",
      "Sami convinced the sub that Sami was the sub.",
      "#36c2cc","students","50% 75%",NULL,26,"sami-x1","Sami · Substitute","General Studies","sami-x1",0,"sami","expansion"},
    { "ravi","Ravi","student","rare","PA System Hack",
      "Ravi played bird calls over the morning announcements.",
      "#ffb05a","students","100% 75%",NULL,27,"ravi-x1","Ravi · PA System","General Studies","ravi-x1",0,"ravi","expansion"},
    { "indra","Indra","student","rare","Graduation Day Ghost",
      "Indra's name was called. The seat was empty. The diploma was gone.",
      "#a06bff","students","0% 75%",NULL,28,"indra-x1","Indra · Graduation","General Studies","indra-x1",0,"indra","expansion"},
    { "mika","Mika","student","rare","Championship Buzzer Beater",
      "Mika didn't look at the clock. She didn't need to.",
      "#52c673","students","50% 75%",NULL,29,"mika-x1","Mika · Buzzer Beater","General Studies","mika-x1",0,"mika","expansion"},
    { "noor","Noor","student","rare","Senior Prank Mastermind",
      "Noor planned it. Nobody can prove it. Everyone knows.",
      "#ec4f9e","students","100% 75%",NULL,30,"noor-x1","Noor · Senior Prank","General Studies","noor-x1",0,"noor","expansion"},
    { "eliza","Eliza","teacher","super-rare","Source Code Lecture",
      "Eliza wrote the textbook, then corrected it in real time.",
      "#62d3c2","specials","50% 25%",NULL,31,"eliza-x1","Eliza · Source Code","Systems","eliza-x1",0,"eliza","expansion"},
    { "rati","Rati","teacher","super-rare","Signal Expansion Permit",
      "Hold the signal. Find the edge. Build there.",
      "#f0a12a","specials","100% 25%",NULL,32,"rati-x1","Rati · Expansion","Signal Studies","rati-x1",0,"rati","expansion"},
    { "captain-null","Captain Null","special","ultra-rare","Page 11 Return Pass",
      "Page 10 got you here. Page 11 gets you back. Maybe.",
      "#222222","specials","0% 25%",NULL,33,"captain-null-x1","Captain Null · Page 11","???","captain-null-x1",0,"captain-null","expansion"},
    { "item-yearbook","Yearbook","item","rare","Senior Yearbook",
      "All four years. Every grade. Every teacher. Every friend who graduated before you.",
      "#8b4513","items","0% 0%",NULL,34,"item-yearbook","Yearbook · Senior","Item","yearbook",0,NULL,NULL},
    { "item-diploma","Diploma","item","ultra-rare","Graduation Diploma",
      "Ruby signed it herself. The ink is still warm.",
      "#d4af37","items","0% 0%",NULL,35,"item-diploma","Diploma · Signed","Item","diploma",0,NULL,NULL},
    { "location-rooftop","Rooftop","location","rare","Senior Rooftop",
      "The door is supposed to be locked. Someone left a chair.",
      "#4a6fa5","locations","0% 0%",NULL,36,"location-rooftop","Rooftop · Senior","Location","rooftop",0,NULL,NULL},
};

const card_profile_t* catalog_by_profile_id(const char *id) {
    for (int i = 0; i < FIRST_BELL_PROFILE_COUNT; i++) {
        /* Simple string match — profile IDs are short ASCII */
        const char *a = FIRST_BELL_CATALOG[i].profile_id;
        const char *b = id;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return &FIRST_BELL_CATALOG[i];
    }
    return NULL;
}

const card_profile_t* catalog_by_set_number(int n) {
    for (int i = 0; i < FIRST_BELL_PROFILE_COUNT; i++) {
        if (FIRST_BELL_CATALOG[i].set_number == n) return &FIRST_BELL_CATALOG[i];
    }
    return NULL;
}

int catalog_card_count(void) {
    return FIRST_BELL_PROFILE_COUNT;
}

void catalog_hash(char out_hex[65]) {
    sha256_ctx_t ctx;
    uint8_t hash[32];
    sha256_init(&ctx);

    /* Hash the catalog in a stable order: set_number, then profile_id, then fields */
    for (int i = 0; i < FIRST_BELL_PROFILE_COUNT; i++) {
        const card_profile_t *c = &FIRST_BELL_CATALOG[i];
        /* set_number as 4-byte LE */
        uint8_t sn[4] = { c->set_number & 0xff, (c->set_number>>8)&0xff, (c->set_number>>16)&0xff, (c->set_number>>24)&0xff };
        sha256_update(&ctx, sn, 4);
        if (c->profile_id) sha256_update(&ctx, c->profile_id, strlen(c->profile_id)+1);
        if (c->character_id) sha256_update(&ctx, c->character_id, strlen(c->character_id)+1);
        if (c->character_name) sha256_update(&ctx, c->character_name, strlen(c->character_name)+1);
        if (c->role) sha256_update(&ctx, c->role, strlen(c->role)+1);
        if (c->rarity) sha256_update(&ctx, c->rarity, strlen(c->rarity)+1);
        if (c->title) sha256_update(&ctx, c->title, strlen(c->title)+1);
        if (c->blurb) sha256_update(&ctx, c->blurb, strlen(c->blurb)+1);
        if (c->subject) sha256_update(&ctx, c->subject, strlen(c->subject)+1);
        uint8_t mintable = c->mintable ? 1 : 0;
        sha256_update(&ctx, &mintable, 1);
    }
    sha256_final(&ctx, hash);
    sha256_hex(hash, out_hex);
}
