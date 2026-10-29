// Bonus gear: restore discontinued rewards (promo makeovers, exclusive rims, Tron/astronaut sets,
// gold rims) while the native item loader reads XML. No archive/save rewriting.
// Off switch: datapack\trials_revive_no_bonus_unlock.txt.
typedef int (__cdecl* PrivateItemIntFn)(void*, const char*);
typedef bool (__fastcall* PrivateItemsLoadFn)(void*, void*, DWORD, void*);
void* g_private_item_int_original;
void* g_private_item_string_original;
void* g_private_items_load_original;
__declspec(thread) int g_private_items_pack; // 1 = base items, 2 = Riders of Doom bikes

bool private_reward_item(int id) {
    if ((id >= 300 && id <= 305) || (id >= 310 && id <= 315)) return true;
    switch (id) {
    case 200: case 210: case 220: case 230: case 240: case 260:
    case 320: case 321: case 322: case 330: case 340:
    case 350: case 351: case 352: case 353: case 354: return true;
    default: return false;
    }
}

bool private_shared_rim(int id) {
    return (id >= 300 && id <= 305) || (id >= 310 && id <= 315) ||
        id == 113 || id == 115 || id == 150 || id == 151 ||
        id == 119 || id == 120 || id == 121 || id == 149;
}

int __cdecl private_item_int(void* node, const char* attribute) {
    PrivateItemIntFn original = (PrivateItemIntFn)g_private_item_int_original;
    int value = original(node, attribute);
    if (g_private_items_pack != 1) return value;
    const bool gate = !strcmp(attribute, "privilegekey") || !strcmp(attribute, "uplayreward") ||
        !strcmp(attribute, "price") || !strcmp(attribute, "medalreq") || !strcmp(attribute, "lisencereq");
    const bool makeover = !strcmp(attribute, "makeover");
    if (!gate && !makeover) return value;
    const int id = original(node, "id");
    const bool gold_rim = id == 273 || id == 274;
    if ((gate && (private_reward_item(id) || gold_rim)) || (makeover && gold_rim)) return 0;
    return value;
}

// Output is uninitialized on entry: construct an owned engine string as the original does.
void* __cdecl private_item_string(void* out, void* node, const char* attribute) {
    if (g_private_items_pack == 1 && (!strcmp(attribute, "biketype") ||
        !strcmp(attribute, "icon") || !strcmp(attribute, "name"))) {
        const int id = ((PrivateItemIntFn)g_private_item_int_original)(node, "id");
        const bool gold = id == 273 || id == 274;
        const char* text = nullptr;
        if (!strcmp(attribute, "biketype"))
            text = private_shared_rim(id) ? "0,1,2,3,6" : (gold ? "2,6" : nullptr);
        // These two pieces were hidden inside the full gold makeover and have no own icons.
        if (gold && !strcmp(attribute, "icon")) text = id == 273 ?
            "bike\\items\\mx\\mx_front_rim_1.png" : "bike\\items\\mx\\mx_back_rim_1.png";
        if (gold && !strcmp(attribute, "name")) text = "BIKE_PART_GOLD";
        if (text) return ((StringFn)STRING_FROM_CSTR)(out, nullptr, text);
    }
    return ((void* (__cdecl*)(void*, void*, const char*))g_private_item_string_original)(out, node, attribute);
}

BYTE* private_rim_category(void* table, BYTE id) {
    // Native ordered hash tables keep owning rows in a list: prev,next,row. Groups only
    // borrow category pointers (the four base motorcycles already share these categories).
    BYTE* map = (BYTE*)table + 0x60;
    BYTE* node = *(BYTE**)(map + 0x14);
    unsigned remaining = *(unsigned*)(map + 0x1c);
    if (remaining > 1024) return nullptr;
    while (node && remaining--) {
        BYTE* row = *(BYTE**)(node + 8);
        if (row && row[0] == id) return row;
        node = *(BYTE**)(node + 4);
    }
    return nullptr;
}

unsigned private_banshee_rims(void* table) {
    BYTE* front = private_rim_category(table, 13);
    BYTE* rear = private_rim_category(table, 15);
    if (!front || !rear) return 0;
    BYTE* map = (BYTE*)table + 0xa0;
    BYTE* node = *(BYTE**)(map + 0x14);
    unsigned remaining = *(unsigned*)(map + 0x1c), changed = 0;
    if (remaining > 1024) return 0;
    while (node && remaining--) {
        BYTE* group = *(BYTE**)(node + 8);
        if (group && *(unsigned short*)(group + 0x28) == 6) {
            unsigned count = *(unsigned*)(group + 0x2c);
            BYTE** categories = *(BYTE***)(group + 0x34);
            if (categories && count <= 256) for (unsigned i = 0; i < count; ++i) {
                if (!categories[i]) continue;
                if (categories[i][0] == 53) { categories[i] = front; ++changed; }
                else if (categories[i][0] == 55) { categories[i] = rear; ++changed; }
            }
        }
        node = *(BYTE**)(node + 4);
    }
    // Original categories 53/55 and their stock item IDs stay registered for old saves.
    return changed;
}

int private_items_context(void* path) {
    if (!path) return 0;
    // Native String has a text pointer at +8; this argument is not a C string.
    const char* text = *(const char* const*)((BYTE*)path + 8);
    if (!text) return 0;
    if (!_stricmp(text, "pack0/items.xml")) return 1;
    if (!_stricmp(text, "pack2/items_bikes.xml") || !_stricmp(text, "pack2/items.xml")) return 2;
    return 0;
}

bool __fastcall private_items_load(void* self, void* edx, DWORD zero, void* path) {
    const int previous = g_private_items_pack;
    const int pack = private_items_context(path);
    bool loaded = false;
    g_private_items_pack = pack;
    __try {
        loaded = ((PrivateItemsLoadFn)g_private_items_load_original)(self, edx, zero, path);
    } __finally {
        g_private_items_pack = previous;
    }
    if (loaded && pack == 1)
        shim_log("private bonus: base reward gates removed; bonus/gold rims compatible with Banshee");
    if (loaded && pack == 2) {
        unsigned changed = private_banshee_rims(self);
        shim_log(changed == 2 ? "private bonus: Banshee front/rear categories use shared rim lists" :
            "private bonus: Banshee rim categories already shared or unavailable");
    }
    return loaded;
}

void start_private_bonus_gear() {
    if (game_file_exists("trials_revive_no_bonus_unlock.txt")) {
        shim_log("bonus gear: unlocks DISABLED by trials_revive_no_bonus_unlock.txt");
        return;
    }
    const BYTE load[7] = {0x6a,0xff,0x68,0x2d,0x41,0xce,0x00};
    const BYTE integer[5] = {0x83,0xec,0x0c,0x53,0x56};
    const BYTE string[7] = {0x6a,0xff,0x68,0x42,0x3d,0xce,0x00};
    if (!GA(0xce412d) || !GA(0xce3d42) ||
        code_differs(ITEMS_LOAD, load, sizeof load) ||
        code_differs(GA(0x6562d0), integer, sizeof integer) ||
        code_differs(GA(0x6565f0), string, sizeof string)) {
        shim_log("private bonus: unexpected item-loader code; hooks skipped");
        return;
    }
    g_private_item_int_original = audio_garage_trampoline(GA(0x6562d0), integer, sizeof integer);
    g_private_item_string_original = audio_garage_trampoline(GA(0x6565f0), string, sizeof string);
    g_private_items_load_original = audio_garage_trampoline(ITEMS_LOAD, load, sizeof load);
    if (g_private_item_int_original && g_private_item_string_original && g_private_items_load_original &&
        patch_jmp(GA(0x6565f0), string, sizeof string, (void*)&private_item_string) &&
        patch_jmp(GA(0x6562d0), integer, sizeof integer, (void*)&private_item_int) &&
        patch_jmp(ITEMS_LOAD, load, sizeof load, (void*)&private_items_load))
        shim_log("private bonus: discontinued reward unlocks and Banshee rims enabled");
    else shim_log("private bonus: item-loader hook installation failed");
}
