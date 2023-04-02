/*This is tone file*/

const char* tone_uri[] = {
   "flash://tone/0_BT_MODE.mp3",
   "flash://tone/1_INTRO_MODE.mp3",
   "flash://tone/2_SD_MODE.mp3",
   "flash://tone/3_WIFI_MODE.mp3",
};

int get_tone_uri_num()
{
    return sizeof(tone_uri) / sizeof(char *) - 1;
}
