/* PicoPlay - window and control data (SymbOS transfer/data segments) */

/* ------------------------------------------------------------ window data */
_transfer char icon[19] = { 0x02, 0x08, 0x08, 0x00, 0x00, 0x33, 0x00, 0x37, 0x08, 0x37, 0x08,
                            0x34, 0x08, 0x34, 0x08, 0x74, 0x08, 0xF0, 0x00 };

#define TXT_TITLE_SIZE 50
_data char txt_title[TXT_TITLE_SIZE];
#define TXT_ARTIST_SIZE 84
_data char txt_artist[TXT_ARTIST_SIZE];
#define TXT_TIME_SIZE 20
_data char txt_time[TXT_TIME_SIZE];
#define TXT_FMT_SIZE 24
_data char txt_fmt[TXT_FMT_SIZE];
#define TXT_PATH_SIZE 64
_data char txt_path[TXT_PATH_SIZE];
#define TXT_STATUS_SIZE 80
_data char txt_status[TXT_STATUS_SIZE];
_data char txt_play[8];

_transfer Ctrl_Text cd_title  = { txt_title,  (COLOR_BLACK << 2) | COLOR_ORANGE | TEXT_FILL, ALIGN_LEFT };
_transfer Ctrl_Text cd_artist = { txt_artist, (COLOR_RED << 2) | COLOR_ORANGE | TEXT_FILL, ALIGN_LEFT };
_transfer Ctrl_Text cd_time   = { txt_time,   (COLOR_BLACK << 2) | COLOR_ORANGE | TEXT_FILL, ALIGN_LEFT };
_transfer Ctrl_Text cd_fmt    = { txt_fmt,    (COLOR_RED << 2) | COLOR_ORANGE | TEXT_FILL, ALIGN_RIGHT };
_transfer Ctrl_Text cd_path   = { txt_path,   (COLOR_BLACK << 2) | COLOR_ORANGE | TEXT_FILL, ALIGN_LEFT };
_transfer Ctrl_Text cd_vol    = { "Vol",      (COLOR_BLACK << 2) | COLOR_ORANGE, ALIGN_LEFT };

_transfer char chk_shuffle = 0;
_transfer char chk_repeat = 0;
_transfer Ctrl_Check cd_shuffle = { &chk_shuffle, "Shuffle", (COLOR_BLACK << 2) | COLOR_ORANGE };
_transfer Ctrl_Check cd_repeat  = { &chk_repeat,  "Repeat",  (COLOR_BLACK << 2) | COLOR_ORANGE };
_transfer Ctrl_Slider cd_volume = { SLIDER_H | SLIDER_VALUE, 0, 80, 100, 5, -5 };

/* folder list: one page of up to LIST_PAGE entries, plus "previous"/"more" rows */
#define LIST_PAGE  120
#define LIST_ROWS  (LIST_PAGE + 2)
#define NAME_LEN   40
_data char row_name[LIST_ROWS][NAME_LEN + 1];
_data char row_info[LIST_ROWS][8];
_transfer List_Row2Col rows[LIST_ROWS];
short row_entry[LIST_ROWS];                 /* engine index, -1 = previous page, -2 = next page */
unsigned char row_type[LIST_ROWS];
unsigned short dir_count, page_first;

_transfer List_Column cols[2] = {
    { ALIGN_LEFT | LTYPE_TEXT, 0, 196, "Name" },
    { ALIGN_RIGHT | LTYPE_TEXT, 0, 40, "Size" }
};
_transfer List cd_list = { 0, 0, rows, 0, 2, 0, cols, 0, LIST_SCROLL };

#define PROG_COLORS ((COLOR_ORANGE << 6) | (COLOR_RED << 4) | (COLOR_BLACK << 2) | COLOR_BLACK)

enum {
    C_BG, C_TITLE, C_ARTIST, C_TIME, C_FMT, C_PROG,
    C_PREV, C_PLAY, C_STOP, C_NEXT, C_SHUF, C_REP, C_VOLT, C_VOL,
    C_PATH, C_LIST, C_COUNT
};

#define WIN_W 252
#define WIN_H 166
#define PROG_X 4
#define PROG_W 244

_transfer Ctrl ctl_bg     = { C_BG,     C_AREA,     -1, COLOR_ORANGE, 0, 0, 10000, 10000 };
_transfer Ctrl ctl_title  = { C_TITLE,  C_TEXT,     -1, (unsigned short)&cd_title, 4, 3, 244, 8 };
_transfer Ctrl ctl_artist = { C_ARTIST, C_TEXT,     -1, (unsigned short)&cd_artist, 4, 13, 244, 8 };
_transfer Ctrl ctl_time   = { C_TIME,   C_TEXT,     -1, (unsigned short)&cd_time, 4, 24, 100, 8 };
_transfer Ctrl ctl_fmt    = { C_FMT,    C_TEXT,     -1, (unsigned short)&cd_fmt, 104, 24, 144, 8 };
_transfer Ctrl ctl_prog   = { C_PROG,   C_PROGRESS, -1, PROG_COLORS, PROG_X, 34, PROG_W, 8 };
_transfer Ctrl ctl_prev   = { C_PREV,   C_BUTTON,   -1, (unsigned short)"|<", 4, 46, 24, 12 };
_transfer Ctrl ctl_play   = { C_PLAY,   C_BUTTON,   -1, (unsigned short)txt_play, 30, 46, 36, 12 };
_transfer Ctrl ctl_stop   = { C_STOP,   C_BUTTON,   -1, (unsigned short)"Stop", 68, 46, 30, 12 };
_transfer Ctrl ctl_next   = { C_NEXT,   C_BUTTON,   -1, (unsigned short)">|", 100, 46, 24, 12 };
_transfer Ctrl ctl_shuf   = { C_SHUF,   C_CHECK,    -1, (unsigned short)&cd_shuffle, 130, 44, 50, 8 };
_transfer Ctrl ctl_rep    = { C_REP,    C_CHECK,    -1, (unsigned short)&cd_repeat, 130, 53, 50, 8 };
_transfer Ctrl ctl_volt   = { C_VOLT,   C_TEXT,     -1, (unsigned short)&cd_vol, 184, 48, 16, 8 };
_transfer Ctrl ctl_vol    = { C_VOL,    C_SLIDER,   -1, (unsigned short)&cd_volume, 200, 48, 48, 8 };
_transfer Ctrl ctl_path   = { C_PATH,   C_TEXT,     -1, (unsigned short)&cd_path, 4, 64, 244, 8 };
_transfer Ctrl ctl_list   = { C_LIST,   C_LISTFULL, -1, (unsigned short)&cd_list, 4, 74, 244, 88 };

_transfer Ctrl_Group ctrls = { C_COUNT, 0, &ctl_bg };

/* menu */
#define M_REFRESH 1
#define M_REMOUNT 2
#define M_DIAG    3
#define M_LOG     4
#define M_QUIT    9
#define M_ABOUT   10
_transfer Menu menu_file = { 6 };
_transfer Menu_Entry mf1 = { MENU_ACTIVE, "Refresh folder", M_REFRESH };
_transfer Menu_Entry mf2 = { MENU_ACTIVE, "Remount microSD", M_REMOUNT };
_transfer Menu_Entry mf3 = { MENU_ACTIVE, "Diagnostics...", M_DIAG };
_transfer Menu_Entry mf4 = { MENU_ACTIVE, "Engine log...", M_LOG };
_transfer Menu_Entry mf5 = { MENU_SEPARATOR, 0, 0 };
_transfer Menu_Entry mf6 = { MENU_ACTIVE, "Quit", M_QUIT };
_transfer Menu menu_help = { 1 };
_transfer Menu_Entry mh1 = { MENU_ACTIVE, "About PicoPlay", M_ABOUT };
_transfer Menu menu_main = { 2 };
_transfer Menu_Entry mm1 = { MENU_ACTIVE | MENU_SUBMENU, "File", (unsigned short)&menu_file };
_transfer Menu_Entry mm2 = { MENU_ACTIVE | MENU_SUBMENU, "Help", (unsigned short)&menu_help };

_transfer Window win_main = {
    WIN_NORMAL,
    WIN_CLOSE | WIN_TITLE | WIN_ICON | WIN_MENU | WIN_STATUS,
    0,
    40, 1,
    WIN_W, WIN_H,
    0, 0,
    WIN_W, WIN_H,
    WIN_W, WIN_H,
    WIN_W, WIN_H,
    icon,
    "PicoPlay",
    txt_status,
    &menu_main,
    &ctrls };

/* text window for diagnostics and the engine log */
#define TEXT_LINES 40
#define TEXT_LEN   60
_data char tline[TEXT_LINES][TEXT_LEN + 1];
_transfer List_Row trows[TEXT_LINES];
_transfer List_Column tcol = { ALIGN_LEFT | LTYPE_TEXT, 0, 300, "" };
_transfer List cd_tlist = { 0, 0, trows, 0, 1, 0, &tcol, 0, LIST_SCROLL };
_transfer Ctrl tctl_bg   = { 0, C_AREA,    -1, COLOR_ORANGE, 0, 0, 10000, 10000 };
_transfer Ctrl tctl_list = { 1, C_LISTBOX, -1, (unsigned short)&cd_tlist, 2, 2, 312, 128 };
_transfer Ctrl tctl_ok   = { 2, C_BUTTON,  -1, (unsigned short)"Close", 134, 134, 48, 12 };
_transfer Ctrl_Group tctrls = { 3, 0, &tctl_bg };
_data char txt_twin[32];
_transfer Window win_text = {
    WIN_NORMAL,
    WIN_CLOSE | WIN_TITLE | WIN_ICON,
    0,
    20, 20,
    316, 150,
    0, 0,
    316, 150,
    316, 150,
    316, 150,
    icon,
    txt_twin,
    0,
    0,
    &tctrls };

/* SCC gives initialized _data char arrays no storage of their own (they all
   land on one address, on top of the next buffer), so the texts are declared
   uninitialized and filled in at startup. */
static void gui_texts_init(void)
{
    strcpy(txt_title, "No track");
    strcpy(txt_artist, "");
    strcpy(txt_time, "--:-- / --:--");
    strcpy(txt_fmt, "");
    strcpy(txt_path, "/");
    strcpy(txt_status, "Looking for the PicoVerse Audio Engine...");
    strcpy(txt_play, "Play");
    strcpy(txt_twin, "Diagnostics");
}
