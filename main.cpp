#include <3ds.h>
#include <citro2d.h>
#include <archive.h>
#include <archive_entry.h>
#include <tremor/ivorbisfile.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

static const char *THEMES_DIR = "sdmc:/themes";
static const char *APP_DIR = "sdmc:/3ds/ShieldThemeAudio";
static const char *PREVIEW_TEMP = "sdmc:/3ds/ShieldThemeAudio/preview.ogg";
static const char *GUIDE_MARKER = "sdmc:/3ds/ShieldThemeAudio/guide.seen";
static const size_t MAX_TRACKS = 512;
static const int MAX_THEME_DEPTH = 8;
static const long MAX_OGG_BYTES = 64L*1024L*1024L;
static const size_t AUDIO_SAMPLES = 4096;
static const int AUDIO_BUFFERS = 4;

struct Track {
    std::string title;
    std::string source;
    std::string entry;
    u64 size;
    bool zipped;
};
struct DestDir { std::string name; std::string path; bool hasMp3; };

static std::vector<Track> tracks;
static std::vector<DestDir> destDirs;
static size_t selected = 0, destSelected = 0, firstVisible = 0;
static bool selectingDestination = false;
static bool choosingFilename = false, confirmOverwrite = false;
static std::string destPath = "sdmc:/";
static std::string pendingOutputPath;
static std::string toast;
static u64 toastUntil = 0;
static C3D_RenderTarget *topTarget = nullptr, *bottomTarget = nullptr;
static C2D_TextBuf textBuf = nullptr;
static OggVorbis_File oggFile;
static bool oggOpen = false, previewing = false, previewEof = false, previewFromTemp = false;
static bool audioReady = false, ndspInitialized = false;
static int previewChannels = 2;
static ndspWaveBuf audioWaves[AUDIO_BUFFERS];
static s16 *audioMemory[AUDIO_BUFFERS] = {nullptr};
static bool guideVisible = false;
static int guidePage = 0;
static bool guideSeen = false;
static bool running = true;

#define RGBA(r,g,b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xff000000u)
static const u32 C_BG=RGBA(5,13,25), C_PANEL=RGBA(10,24,42), C_PANEL2=RGBA(16,39,62);
static const u32 C_EDGE=RGBA(39,67,93), C_CYAN=RGBA(47,211,242), C_WHITE=RGBA(246,249,255);
static const u32 C_MUTED=RGBA(165,184,205), C_GREEN=RGBA(91,224,171), C_GOLD=RGBA(255,199,91);

static void setToast(const char *s, u64 ms=2600) { toast=s?s:""; toastUntil=osGetTime()+ms; }
static std::string joinPath(const std::string &a, const std::string &b) {
    if(a.empty() || a=="sdmc:/") return a+b;
    return a+"/"+b;
}
static bool isDir(const std::string &path) { struct stat st; return stat(path.c_str(),&st)==0 && S_ISDIR(st.st_mode); }
static bool endsWithI(const std::string &s,const char *suffix) {
    size_t n=strlen(suffix); return s.size()>=n && strcasecmp(s.c_str()+s.size()-n,suffix)==0;
}
static std::string leafName(const std::string &path) {
    size_t p=path.find_last_of('/'); return p==std::string::npos?path:path.substr(p+1);
}
static std::string themeTitle(std::string name) {
    if(endsWithI(name,".zip")) name.resize(name.size()-4);
    for(char &c:name) if(c=='_' || c=='-') c=' ';
    while(!name.empty() && (name.back()=='.' || name.back()==' ')) name.pop_back();
    if(name.empty()) name="Theme sans nom";
    return name;
}
static std::string fitText(const std::string &s,size_t maxCodepoints) {
    size_t i=0, count=0;
    while(i<s.size() && count<maxCodepoints) {
        unsigned char c=(unsigned char)s[i]; size_t n=(c<0x80)?1:((c&0xE0)==0xC0?2:((c&0xF0)==0xE0?3:((c&0xF8)==0xF0?4:1)));
        if(i+n>s.size()) n=1;
        i+=n; count++;
    }
    if(i>=s.size()) return s;
    return s.substr(0,i)+"...";
}
static void drawText(const std::string &s,float x,float y,float scale,u32 color) {
    C2D_Text t; C2D_TextParse(&t,textBuf,s.c_str());
    C2D_DrawText(&t,C2D_WithColor,x,y,0.5f,scale,scale,color);
}
static void rect(float x,float y,float w,float h,u32 color) { C2D_DrawRectSolid(x,y,0.5f,w,h,color); }
static void circle(float x,float y,float r,u32 color) { C2D_DrawCircleSolid(x,y,0.5f,r,color); }
static void musicNote(float x,float y,float scale,u32 color) {
    circle(x+4*scale,y+17*scale,4*scale,color);
    circle(x+15*scale,y+13*scale,4*scale,color);
    rect(x+7*scale,y+2*scale,2*scale,15*scale,color);
    rect(x+18*scale,y-2*scale,2*scale,15*scale,color);
    rect(x+7*scale,y-2*scale,13*scale,3*scale,color);
}

static bool isBgmName(const char *path) {
    if(!path) return false;
    const char *base=strrchr(path,'/'); base=base?base+1:path;
    return strcasecmp(base,"BGM.ogg")==0;
}
static bool zipFindBgm(const std::string &zip,std::string &entry,u64 &fileSize) {
    struct archive *a=archive_read_new(); if(!a) return false;
    archive_read_support_filter_none(a); archive_read_support_format_zip(a);
    bool found=false;
    if(archive_read_open_filename(a,zip.c_str(),16384)==ARCHIVE_OK) {
        struct archive_entry *e=nullptr;
        while(archive_read_next_header(a,&e)==ARCHIVE_OK) {
            const char *p=archive_entry_pathname(e);
            la_int64_t size=archive_entry_size(e);
            if(archive_entry_filetype(e)==AE_IFREG && isBgmName(p) && size>0 && size<=MAX_OGG_BYTES) { entry=p; fileSize=(u64)size; found=true; break; }
            archive_read_data_skip(a);
        }
        archive_read_close(a);
    }
    archive_read_free(a); return found;
}
static bool extractZipEntry(const std::string &zip,const std::string &entry,const std::string &outPath) {
    struct archive *a=archive_read_new(); if(!a) return false;
    archive_read_support_filter_none(a); archive_read_support_format_zip(a);
    bool ok=false;
    if(archive_read_open_filename(a,zip.c_str(),16384)==ARCHIVE_OK) {
        struct archive_entry *e=nullptr;
        while(archive_read_next_header(a,&e)==ARCHIVE_OK) {
            const char *p=archive_entry_pathname(e);
            if(p && entry==p) {
                la_int64_t size=archive_entry_size(e);
                if(size<=0 || size>MAX_OGG_BYTES) break;
                FILE *out=fopen(outPath.c_str(),"wb");
                if(out) {
                    char buf[16384]; la_ssize_t n; ok=true;
                    while((n=archive_read_data(a,buf,sizeof(buf)))>0) if(fwrite(buf,1,(size_t)n,out)!=(size_t)n) { ok=false; break; }
                    if(n<0) ok=false;
                    if(fclose(out)!=0) ok=false;
                }
                break;
            }
            archive_read_data_skip(a);
        }
        archive_read_close(a);
    }
    archive_read_free(a); if(!ok) remove(outPath.c_str()); return ok;
}
static void scanThemeFolder(const std::string &folder,int depth) {
    if(depth>MAX_THEME_DEPTH || tracks.size()>=MAX_TRACKS) return;
    DIR *d=opendir(folder.c_str()); if(!d) return;
    struct dirent *it;
    while((it=readdir(d)) && tracks.size()<MAX_TRACKS) {
        if(it->d_name[0]=='.') continue;
        std::string p=joinPath(folder,it->d_name);
        if(isDir(p)) {
            std::string bgm=joinPath(p,"BGM.ogg"); struct stat st;
            if(stat(bgm.c_str(),&st)==0 && S_ISREG(st.st_mode)) tracks.push_back({themeTitle(it->d_name),bgm,"",(u64)st.st_size,false});
            scanThemeFolder(p,depth+1);
        } else if(endsWithI(it->d_name,".zip")) {
            std::string entry; u64 fileSize=0;
            if(zipFindBgm(p,entry,fileSize)) tracks.push_back({themeTitle(it->d_name),p,entry,fileSize,true});
        } else if(strcasecmp(it->d_name,"BGM.ogg")==0 && depth==0) {
            struct stat st; u64 fileSize=(stat(p.c_str(),&st)==0 && S_ISREG(st.st_mode))?(u64)st.st_size:0;
            tracks.push_back({"Thème sans nom",p,"",fileSize,false});
        }
    }
    closedir(d);
}
static void scanThemes() {
    tracks.clear(); DIR *d=opendir(THEMES_DIR);
    if(!d) { mkdir(THEMES_DIR,0777); setToast("Dossier SD:/themes introuvable : créé.",3600); return; }
    closedir(d);
    scanThemeFolder(THEMES_DIR,0);
    std::sort(tracks.begin(),tracks.end(),[](const Track&a,const Track&b){return strcasecmp(a.title.c_str(),b.title.c_str())<0;});
    selected=0; firstVisible=0;
    if(tracks.empty()) setToast("Aucun BGM.ogg trouvé dans SD:/themes.",3600);
    else setToast((std::to_string(tracks.size())+" thème(s) musical(aux) trouvé(s).").c_str());
}

static void stopPreview() {
    if(ndspInitialized) ndspChnReset(0);
    memset(audioWaves,0,sizeof(audioWaves));
    if(oggOpen) { ov_clear(&oggFile); oggOpen=false; }
    previewing=false; previewEof=false;
    if(previewFromTemp) { remove(PREVIEW_TEMP); previewFromTemp=false; }
}
static bool startPreview(const Track &t) {
    stopPreview();
    if(!audioReady) { setToast("Audio de la 3DS indisponible."); return false; }
    bool ready=t.zipped?extractZipEntry(t.source,t.entry,PREVIEW_TEMP):true;
    const char *path=t.zipped?PREVIEW_TEMP:t.source.c_str();
    FILE *audioFile=ready?fopen(path,"rb"):nullptr;
    if(!audioFile || ov_open(audioFile,&oggFile,nullptr,0)<0) { if(audioFile) fclose(audioFile); if(t.zipped) remove(PREVIEW_TEMP); setToast("Impossible d'ouvrir ce BGM.ogg."); return false; }
    oggOpen=true; previewFromTemp=t.zipped;
    vorbis_info *info=ov_info(&oggFile,-1);
    if(!info || (info->channels!=1 && info->channels!=2) || info->rate<8000 || info->rate>96000) {
        stopPreview(); setToast("Format OGG non pris en charge."); return false;
    }
    previewChannels=info->channels;
    ndspChnReset(0); ndspChnSetInterp(0,NDSP_INTERP_LINEAR);
    ndspChnSetRate(0,(float)info->rate);
    ndspChnSetFormat(0,previewChannels==1?NDSP_FORMAT_MONO_PCM16:NDSP_FORMAT_STEREO_PCM16);
    float mix[12]={1.0f,1.0f,0,0,0,0,0,0,0,0,0,0}; ndspChnSetMix(0,mix);
    previewing=true; previewEof=false; setToast("Préécoute en cours. Y ou B pour arrêter."); return true;
}
static void pumpPreview() {
    if(!previewing || !oggOpen) return;
    for(int i=0;i<AUDIO_BUFFERS;i++) {
        if(audioWaves[i].status==NDSP_WBUF_QUEUED || audioWaves[i].status==NDSP_WBUF_PLAYING) continue;
        int bitstream=0; long bytes=ov_read(&oggFile,(char*)audioMemory[i],(int)(AUDIO_SAMPLES*previewChannels*2),&bitstream);
        if(bytes>0) {
            DSP_FlushDataCache(audioMemory[i],(u32)bytes);
            memset(&audioWaves[i],0,sizeof(audioWaves[i]));
            audioWaves[i].data_vaddr=audioMemory[i];
            audioWaves[i].nsamples=(u32)(bytes/(previewChannels*2));
            ndspChnWaveBufAdd(0,&audioWaves[i]);
        } else if(bytes==0) previewEof=true;
    }
    if(previewEof) {
        bool busy=false;
        for(int i=0;i<AUDIO_BUFFERS;i++) if(audioWaves[i].status==NDSP_WBUF_QUEUED || audioWaves[i].status==NDSP_WBUF_PLAYING) busy=true;
        if(!busy) stopPreview();
    }
}

static bool copyFileTo(const std::string &src,const std::string &dest) {
    FILE *in=fopen(src.c_str(),"rb"), *out=fopen(dest.c_str(),"wb");
    if(!in || !out) { if(in) fclose(in); if(out) fclose(out); return false; }
    char buf[32768]; size_t n; bool ok=true;
    while((n=fread(buf,1,sizeof(buf),in))>0) if(fwrite(buf,1,n,out)!=n) {ok=false;break;}
    if(ferror(in)) ok=false;
    fclose(in);
    if(fclose(out)!=0) ok=false;
    if(!ok) remove(dest.c_str());
    return ok;
}
static bool copyZipEntryTo(const Track &t,const std::string &dest) {
    struct archive *a=archive_read_new(); if(!a) return false;
    archive_read_support_filter_none(a); archive_read_support_format_zip(a);
    bool ok=false;
    if(archive_read_open_filename(a,t.source.c_str(),16384)==ARCHIVE_OK) {
        struct archive_entry *e=nullptr;
        while(archive_read_next_header(a,&e)==ARCHIVE_OK) {
            const char *p=archive_entry_pathname(e);
            if(p && t.entry==p) {
                la_int64_t size=archive_entry_size(e);
                if(size<=0 || size>MAX_OGG_BYTES) break;
                FILE *out=fopen(dest.c_str(),"wb");
                if(out) {
                    char buf[32768]; la_ssize_t n; ok=true;
                    while((n=archive_read_data(a,buf,sizeof(buf)))>0) if(fwrite(buf,1,(size_t)n,out)!=(size_t)n) {ok=false;break;}
                    if(n<0) ok=false;
                    if(fclose(out)!=0) ok=false;
                }
                break;
            }
            archive_read_data_skip(a);
        }
        archive_read_close(a);
    }
    archive_read_free(a); if(!ok) remove(dest.c_str()); return ok;
}
static std::string safeFilename(std::string s) {
    for(char &c:s) if((unsigned char)c<32 || strchr("\\/:*?\"<>|",c)) c='_';
    while(!s.empty() && (s.back()=='.' || s.back()==' ')) s.pop_back();
    if(s.empty()) s="Theme audio";
    return s;
}
static bool copySelectedTo(const std::string &outputPath) {
    if(selected>=tracks.size()) return false;
    const Track &t=tracks[selected];
    return t.zipped?copyZipEntryTo(t,outputPath):copyFileTo(t.source,outputPath);
}

static bool folderHasMp3(const std::string &path) {
    DIR *d=opendir(path.c_str()); if(!d) return false;
    struct dirent *it; bool found=false;
    while((it=readdir(d))) if(endsWithI(it->d_name,".mp3")) { found=true; break; }
    closedir(d); return found;
}
static void readDestDirs() {
    destDirs.clear(); DIR *d=opendir(destPath.c_str()); if(!d) return;
    struct dirent *it;
    while((it=readdir(d))) {
        if(it->d_name[0]=='.') continue;
        std::string p=joinPath(destPath,it->d_name);
        if(isDir(p)) destDirs.push_back({it->d_name,p,folderHasMp3(p)});
    }
    closedir(d);
    std::sort(destDirs.begin(),destDirs.end(),[](const DestDir&a,const DestDir&b){if(a.hasMp3!=b.hasMp3)return a.hasMp3>b.hasMp3;return strcasecmp(a.name.c_str(),b.name.c_str())<0;});
    destSelected=0;
}
static std::string parentPath(const std::string &path) {
    if(path=="sdmc:/" || path=="sdmc:") return "sdmc:/";
    size_t end=path.size(); while(end>6 && path[end-1]=='/') end--;
    size_t slash=path.find_last_of('/',end-1);
    if(slash==std::string::npos || slash<5) return "sdmc:/";
    return path.substr(0,slash);
}
static void enterDestination() { selectingDestination=true; choosingFilename=false; confirmOverwrite=false; firstVisible=0; destPath="sdmc:/"; readDestDirs(); }
static void chooseDestination() { selectingDestination=false; choosingFilename=true; }
static void beginCopy(bool useThemeName) {
    if(selected>=tracks.size()) return;
    std::string filename=useThemeName?safeFilename(tracks[selected].title)+".ogg":"BGM.ogg";
    pendingOutputPath=joinPath(destPath,filename);
    if(access(pendingOutputPath.c_str(),F_OK)==0) { confirmOverwrite=true; choosingFilename=false; return; }
    stopPreview();
    if(copySelectedTo(pendingOutputPath)) setToast(("Copié : "+filename).c_str(),3600);
    else setToast("Échec de copie. Vérifie la carte SD.",3600);
    choosingFilename=false; confirmOverwrite=false;
}
static void overwriteCopy() {
    stopPreview();
    if(copySelectedTo(pendingOutputPath)) setToast(("Copié : "+leafName(pendingOutputPath)).c_str(),3600);
    else setToast("Échec de copie. Vérifie la carte SD.",3600);
    confirmOverwrite=false; choosingFilename=false;
}

static void drawHeader(const char *title,const char *right) {
    drawText("SHIELD",22,7,0.48f,C_WHITE);
    drawText(title,100,7,0.48f,C_CYAN);
    drawText(right,103,27,0.30f,C_WHITE);
    rect(20,46,360,1,C_CYAN);
}
static void drawTopScreen() {
    C2D_TargetClear(topTarget,C_BG); C2D_SceneBegin(topTarget);
    drawHeader("THEME MUSIC","EXTRACTOR");
    rect(16,52,368,174,C_PANEL); rect(16,52,2,174,C_CYAN); rect(382,52,2,174,C_CYAN);
    if(confirmOverwrite) {
        drawText("FICHIER DÉJÀ EXISTANT",31,65,0.45f,C_GOLD);
        drawText(fitText(leafName(pendingOutputPath),32),31,99,0.48f,C_WHITE);
        drawText("A : écraser le fichier",31,141,0.38f,C_GREEN);
        drawText("B : annuler et revenir",31,168,0.38f,C_MUTED);
    } else if(choosingFilename) {
        drawText("NOM DU FICHIER",31,66,0.45f,C_CYAN);
        drawText(fitText(tracks[selected].title,27),31,97,0.48f,C_WHITE);
        drawText("A : nom du thème.ogg",31,134,0.38f,C_GREEN);
        drawText("X : conserver BGM.ogg",31,161,0.38f,C_WHITE);
        drawText("B : retour au dossier",31,188,0.35f,C_MUTED);
    } else if(selectingDestination) {
        drawText("CHOISIR LE DOSSIER",31,66,0.48f,C_CYAN);
        drawText(fitText(destPath,45),31,95,0.50f,C_WHITE);
        drawText("Les dossiers contenant des MP3",31,130,0.39f,C_MUTED);
        drawText("apparaissent en premier.",31,153,0.39f,C_MUTED);
        drawText("A : ouvrir",31,188,0.36f,C_WHITE);
        drawText("X : choisir ce dossier",217,188,0.36f,C_GREEN);
    } else if(selected<tracks.size()) {
        const Track &t=tracks[selected];
        rect(28,70,64,66,C_PANEL2); rect(28,70,64,1,C_EDGE); rect(28,135,64,1,C_EDGE);
        rect(28,70,1,66,C_EDGE); rect(91,70,1,66,C_EDGE);
        musicNote(45,81,1.35f,C_CYAN);
        drawText("BGM.ogg",105,65,0.55f,C_WHITE); rect(105,91,260,1,C_EDGE);
        drawText("FROM:",105,101,0.32f,C_CYAN); drawText(fitText(t.title,27),150,99,0.35f,C_WHITE);
        drawText("Type:",105,123,0.32f,C_CYAN); drawText(t.zipped?"ZIP":"Dossier",150,121,0.34f,C_WHITE);
        char sizeText[32];
        if(t.size >= 1024u*1024u) snprintf(sizeText,sizeof(sizeText),"%.1f MB",(double)t.size/(1024.0*1024.0));
        else snprintf(sizeText,sizeof(sizeText),"%llu KB",(unsigned long long)((t.size+1023u)/1024u));
        drawText("Size:",105,145,0.32f,C_CYAN); drawText(sizeText,150,143,0.34f,C_WHITE);
        drawText("Status:",105,167,0.32f,C_CYAN);
        drawText(previewing?"Playing":"Ready to copy",150,165,0.34f,previewing?C_GREEN:C_WHITE);
        rect(29,194,336,1,C_EDGE);
        drawText(previewing?"Y : ARRÊTER L'ÉCOUTE":"Y : ÉCOUTER AVANT DE COPIER",32,201,0.28f,C_CYAN);
    } else {
        drawText("AUCUN BGM.OGG",31,74,0.55f,C_WHITE);
        drawText("Place les thèmes Anemone dans",31,113,0.39f,C_MUTED);
        drawText("SD:/themes puis appuie sur X.",31,140,0.39f,C_MUTED);
        drawText("ZIP et dossiers sont pris en charge.",31,168,0.36f,C_MUTED);
    }
    if(!toast.empty() && osGetTime()<toastUntil) {
        rect(22,178,356,34,RGBA(19,51,73));
        drawText(fitText(toast,54),31,185,0.31f,C_WHITE);
    }
}
static void drawMainBottom() {
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    musicNote(14,8,0.55f,C_CYAN); drawText("MUSIQUES TROUVÉES",31,7,0.37f,C_WHITE);
    char count[32]; snprintf(count,sizeof(count),"(%u)",(unsigned)tracks.size()); drawText(count,221,8,0.33f,C_CYAN);
    rect(10,28,300,1,C_CYAN);
    const int rows=5; if(selected<firstVisible) firstVisible=selected; if(selected>=firstVisible+rows) firstVisible=selected-rows+1;
    for(int row=0;row<rows;row++) {
        size_t i=firstVisible+(size_t)row; if(i>=tracks.size()) break;
        float y=33+row*27.0f;
        rect(12,y,276,24,i==selected?C_CYAN:C_PANEL);
        rect(12,y,i==selected?2:1,24,i==selected?C_CYAN:C_EDGE);
        musicNote(20,y+3,0.48f,i==selected?C_BG:C_CYAN);
        drawText(fitText(tracks[i].title,23),41,y+3,0.32f,i==selected?C_BG:C_WHITE);
        drawText(tracks[i].zipped?"ZIP":"DIR",258,y+5,0.23f,i==selected?C_BG:C_MUTED);
    }
    rect(17,172,136,30,C_PANEL2); rect(17,172,136,1,C_CYAN); rect(17,201,136,1,C_CYAN);
    rect(17,172,1,30,C_CYAN); rect(152,172,1,30,C_CYAN); rect(30,180,10,12,C_CYAN); rect(32,178,10,12,C_CYAN); drawText("COPIER",57,180,0.31f,C_WHITE);
    rect(167,172,136,30,C_PANEL2); rect(167,172,136,1,C_CYAN); rect(167,201,136,1,C_CYAN);
    rect(167,172,1,30,C_CYAN); rect(302,172,1,30,C_CYAN); rect(185,181,18,11,C_CYAN); rect(188,178,12,3,C_CYAN); drawText("DOSSIER",213,180,0.31f,C_WHITE);
    rect(10,211,300,1,C_EDGE);
    drawText("A : Choisir     Y : Écouter     B : Retour",16,218,0.24f,C_WHITE);
}
static void drawDestBottom() {
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    drawText("DOSSIERS DE DESTINATION",12,8,0.34f,C_WHITE);
    rect(10,31,300,1,C_EDGE);
    const int rows=5;
    for(int row=0;row<rows;row++) {
        size_t i=firstVisible+(size_t)row; if(i>=destDirs.size()) break;
        float y=38+row*34.0f;
        if(y>207) break;
        rect(10,y,300,31,i==destSelected?C_PANEL2:C_PANEL);
        rect(10,y,i==destSelected?3:1,31,i==destSelected?C_CYAN:C_EDGE);
        drawText(fitText(destDirs[i].name,25),20,y+6,0.34f,i==destSelected?C_WHITE:C_MUTED);
        if(destDirs[i].hasMp3) drawText("MP3",267,y+8,0.25f,C_GOLD);
    }
    rect(10,221,300,1,C_EDGE);
    drawText("A Ouvrir   X Choisir ici   B Retour",12,226,0.26f,C_MUTED);
}
static void drawCopyBottom() {
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    rect(10,22,300,190,C_PANEL);
    if(confirmOverwrite) {
        drawText("CONFIRMATION",24,40,0.43f,C_GOLD);
        drawText("Le fichier existe déjà.",24,78,0.34f,C_WHITE);
        drawText("A  Écraser",24,122,0.40f,C_GREEN);
        drawText("B  Annuler",24,157,0.38f,C_MUTED);
    } else {
        drawText("NOM DE LA COPIE",24,40,0.43f,C_CYAN);
        drawText("A  Nom du thème.ogg",24,88,0.36f,C_GREEN);
        drawText("X  BGM.ogg",24,123,0.36f,C_WHITE);
        drawText("B  Retour au choix du dossier",24,166,0.30f,C_MUTED);
    }
}
static void drawGuide() {
    C2D_TargetClear(topTarget,C_BG); C2D_SceneBegin(topTarget); drawHeader("GUIDE RAPIDE","3 PAGES");
    rect(22,58,356,150,C_PANEL); rect(22,58,4,150,C_CYAN);
    static const char *titles[]={"TROUVE TES MUSIQUES","ÉCOUTE AVANT DE COPIER","CHOISIS TA DESTINATION"};
    static const char *lines[3][3]={
        {"Les thèmes sont recherchés dans","SD:/themes et ses sous-dossiers,","en ZIP ou en dossiers Anemone."},
        {"Sélectionne un thème dans la liste.","Appuie sur Y pour lancer ou arrêter","la préécoute de BGM.ogg."},
        {"Appuie sur A pour choisir le dossier.","Les dossiers avec MP3 passent avant.","Choisis ensuite le nom de la copie."}
    };
    drawText(titles[guidePage],39,78,0.43f,C_CYAN);
    for(int i=0;i<3;i++) drawText(lines[guidePage][i],39,111+i*22,0.38f,C_WHITE);
    char p[24]; snprintf(p,sizeof(p),"PAGE %d / 3",guidePage+1); drawText(p,39,182,0.28f,C_MUTED);
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    rect(10,18,300,195,C_PANEL); drawText("COMMANDES",24,32,0.47f,C_WHITE);
    drawText("A  Valider / continuer / copier",24,72,0.36f,C_GREEN);
    drawText("B  Retour / annuler",24,105,0.38f,C_WHITE);
    if(guidePage==2) drawText("X  Choisir le dossier affiché",24,130,0.33f,C_CYAN);
    drawText("SELECT au démarrage : revoir le guide",24,173,0.31f,C_MUTED);
    drawText("A  OK / suivant       B  retour",20,222,0.31f,C_CYAN);
}
static void drawAll() {
    C2D_TextBufClear(textBuf);
    if(guideVisible) drawGuide();
    else { drawTopScreen(); if(choosingFilename || confirmOverwrite) drawCopyBottom(); else if(selectingDestination) drawDestBottom(); else drawMainBottom(); }
}

static void finishGuide() {
    guideVisible=false; guideSeen=true; FILE *f=fopen(GUIDE_MARKER,"wb"); if(f){fputc(1,f);fclose(f);}
    if(tracks.empty()) scanThemes();
}
static void handleGuide(u32 keys) {
    if(keys&KEY_A) { if(guidePage<2) guidePage++; else finishGuide(); }
    if(keys&KEY_B) { if(guidePage>0) guidePage--; else finishGuide(); }
}
static void handleMain(u32 keys) {
    if(confirmOverwrite) {
        if(keys&KEY_A) overwriteCopy();
        if(keys&KEY_B) { confirmOverwrite=false; choosingFilename=true; }
        return;
    }
    if(choosingFilename) {
        if(keys&KEY_A) beginCopy(true);
        if(keys&KEY_X) beginCopy(false);
        if(keys&KEY_B) { choosingFilename=false; selectingDestination=true; }
        return;
    }
    if(selectingDestination) {
        if(keys&KEY_B) {
            if(destPath=="sdmc:/") selectingDestination=false;
            else { destPath=parentPath(destPath); readDestDirs(); firstVisible=0; }
        }
        if(keys&KEY_UP) { if(destDirs.empty() && destPath!="sdmc:/") destSelected=0; else if(destSelected>0) destSelected--; }
        if(keys&KEY_DOWN) { if(destSelected<destDirs.size()) destSelected++; }
        if(destSelected<firstVisible) firstVisible=destSelected;
        if(destSelected>=firstVisible+5) firstVisible=destSelected-4;
        if((keys&KEY_A) && destSelected<destDirs.size()) { destPath=destDirs[destSelected].path; readDestDirs(); firstVisible=0; }
        if(keys&KEY_X) chooseDestination();
        return;
    }
    if(keys&KEY_X) scanThemes();
    if(keys&KEY_UP && selected>0) { selected--; if(previewing) stopPreview(); }
    if(keys&KEY_DOWN && selected+1<tracks.size()) { selected++; if(previewing) stopPreview(); }
    if(keys&KEY_B) stopPreview();
    if((keys&KEY_Y) && selected<tracks.size()) { if(previewing) stopPreview(); else startPreview(tracks[selected]); }
    if((keys&KEY_A) && selected<tracks.size()) enterDestination();
}
int main() {
    gfxInitDefault(); consoleInit(GFX_BOTTOM,nullptr);
    if(!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { gfxExit(); return 1; }
    C2D_Prepare(); topTarget=C2D_CreateScreenTarget(GFX_TOP,GFX_LEFT); bottomTarget=C2D_CreateScreenTarget(GFX_BOTTOM,GFX_LEFT); textBuf=C2D_TextBufNew(8192);
    mkdir("sdmc:/3ds",0777); mkdir(APP_DIR,0777); mkdir(THEMES_DIR,0777);
    ndspInitialized=R_SUCCEEDED(ndspInit()); audioReady=ndspInitialized; if(audioReady) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    for(int i=0;i<AUDIO_BUFFERS;i++) { audioMemory[i]=(s16*)linearAlloc(AUDIO_SAMPLES*2*sizeof(s16)); memset(&audioWaves[i],0,sizeof(audioWaves[i])); if(!audioMemory[i]) audioReady=false; }
    FILE *marker=fopen(GUIDE_MARKER,"rb"); guideSeen=marker!=nullptr; if(marker) fclose(marker);
    hidScanInput(); bool review=(hidKeysHeld()&KEY_SELECT)!=0;
    if(!guideSeen || review) { guideVisible=true; guidePage=0; }
    else scanThemes();
    while(aptMainLoop() && running) {
        hidScanInput(); u32 keys=hidKeysDown();
        if(guideVisible) handleGuide(keys);
        else { if(keys&KEY_START) running=false; else handleMain(keys); }
        pumpPreview();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW); drawAll(); C2D_Flush(); C3D_FrameEnd(0);
        gspWaitForVBlank();
    }
    stopPreview(); for(int i=0;i<AUDIO_BUFFERS;i++) if(audioMemory[i]) linearFree(audioMemory[i]);
    if(ndspInitialized) ndspExit();
    C2D_TextBufDelete(textBuf); C2D_Fini(); C3D_Fini(); gfxExit(); return 0;
}
