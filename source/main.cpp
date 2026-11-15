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
#include "../include/shield_language_choice.h"

static const char *THEMES_DIR = "sdmc:/themes";
static const char *APP_DIR = "sdmc:/3ds/ShieldThemeAudio";
static const char *PREVIEW_TEMP = "sdmc:/3ds/ShieldThemeAudio/preview.ogg";
static const char *GUIDE_MARKER = "sdmc:/3ds/ShieldThemeAudio/guide.seen";
static const size_t MAX_TRACKS = 512;
static const size_t MAX_DEST_DIRS = 128;
static const size_t MAX_SD_PATH_BYTES = 0x300;
static const int MAX_THEME_DEPTH = 8;
static const long MAX_OGG_BYTES = 64L*1024L*1024L;
static const size_t MAX_PREVIEW_RAM_BYTES = 8u*1024u*1024u;
static const size_t AUDIO_SAMPLES = 8192;
static const int AUDIO_BUFFERS = 6;

struct Track {
    std::string title;
    std::string source;
    std::string entry;
    u64 size;
    bool zipped;
};
struct DestDir { std::string name; std::string path; bool hasMp3; };
struct PreviewMemorySource { unsigned char *data; size_t size; size_t offset; };

static std::vector<Track> tracks;
// The SD scan runs on a worker so the loading screen can keep rendering on
// Old 3DS while ZIP archives are inspected.
static std::vector<Track> scanResults;
static Thread scanWorkerThread = nullptr;
static volatile bool scanFinished = false;
static volatile u32 scanEntriesVisited = 0;
static bool scanRootMissing = false;
static bool loadingVisible = false;
static bool showGuideAfterScan = false;
static u64 scanStartedAt = 0;
static std::vector<DestDir> destDirs;
static size_t selected = 0, destSelected = 0, firstVisible = 0;
static bool selectingDestination = false;
static bool choosingFilename = false, confirmOverwrite = false;
enum CopyResultState { COPY_RESULT_NONE, COPY_RESULT_SUCCESS, COPY_RESULT_FAILED, COPY_RESULT_CANCELLED };
static CopyResultState copyResult = COPY_RESULT_NONE;
static std::string copyResultDetail;
static std::string destPath = "sdmc:/";
static std::string pendingOutputPath;
static std::string toast;
static u64 toastUntil = 0;
static C3D_RenderTarget *topTarget = nullptr, *bottomTarget = nullptr;
static C2D_TextBuf textBuf = nullptr;
static C2D_Font uiFont = nullptr;
static bool topTextContext = false;
static bool bottomTextContext = false;
static OggVorbis_File oggFile;
static PreviewMemorySource previewMemorySource = {nullptr,0,0};
static bool oggOpen = false, previewing = false, previewFromTemp = false;
static volatile bool previewStopRequested = false, previewEof = false, previewDecodeError = false;
static bool audioReady = false, ndspInitialized = false;
static int previewChannels = 2;
static long previewRate = 0;
static ndspWaveBuf audioWaves[AUDIO_BUFFERS];
static s16 *audioMemory[AUDIO_BUFFERS] = {nullptr};
static Thread previewWorkerThread = nullptr;
static bool guideVisible = false;
static int guidePage = 0;
static bool guideSeen = false;
static ShieldLanguageChoice uiLanguage;
static bool languagePromptVisible = false;
static bool languagePromptAfterScan = false;
static bool running = true;

#define RGBA(r,g,b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xff000000u)
static const u32 C_BG=RGBA(5,13,25), C_PANEL=RGBA(10,24,42), C_PANEL2=RGBA(16,39,62);
static const u32 C_EDGE=RGBA(39,67,93), C_CYAN=RGBA(47,211,242), C_WHITE=RGBA(246,249,255);
static const u32 C_MUTED=RGBA(165,184,205), C_GREEN=RGBA(91,224,171), C_GOLD=RGBA(255,199,91);
static const u32 C_RED=RGBA(245,91,111);

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
static std::string translateUiText(const std::string &s) {
    if (uiLanguage.french) return s;
    struct Entry { const char *fr; const char *en; };
    static const Entry entries[] = {
        {"FICHIER DÉJÀ EXISTANT","FILE ALREADY EXISTS"},
        {"A : écraser le fichier","A: replace file"}, {"B : annuler et revenir","B: cancel and go back"},
        {"NOM DU FICHIER","FILE NAME"}, {"A : nom du thème.ogg","A: theme name.ogg"},
        {"X : conserver BGM.ogg","X: keep BGM.ogg"}, {"B : retour au dossier","B: back to folder"},
        {"CHOISIR LE DOSSIER","CHOOSE FOLDER"}, {"Les dossiers contenant des MP3","Folders containing MP3 files"},
        {"apparaissent en premier.","are listed first."}, {"A : ouvrir","A: open"}, {"X : choisir ce dossier","X: choose this folder"},
        {"DEPUIS :","FROM:"}, {"Format :","Format:"}, {"Dossier","Folder"},
        {"Taille :","Size:"}, {"État :","Status:"}, {"En lecture","Playing"}, {"Prête à copier","Ready to copy"},
        {"Y : ARRÊTER L'ÉCOUTE","Y: STOP PREVIEW"}, {"Y : ÉCOUTER AVANT DE COPIER","Y: PREVIEW BEFORE COPYING"},
        {"AUCUN BGM.OGG","NO BGM.OGG FOUND"}, {"Place les thèmes Anemone dans","Put themes in"},
        {"SD:/themes puis appuie sur X.","SD:/themes, then press X."}, {"ZIP et dossiers sont pris en charge.","ZIP files and folders are supported."},
        {"MUSIQUES TROUVÉES","MUSIC FOUND"}, {"COPIER","COPY"}, {"DOSSIER","FOLDER"},
        {"A : Choisir     Y : Écouter     B : Retour","A: Select   Y: Preview   B: Back"},
        {"DOSSIERS DE DESTINATION","DESTINATION FOLDERS"}, {"A : ouvrir   X : choisir   B : retour","A: Open   X: Choose   B: Back"},
        {"CONFIRMATION","CONFIRM"}, {"Le fichier existe déjà.","This file already exists."},
        {"A  Écraser","A  Replace"}, {"B  Annuler","B  Cancel"}, {"NOM DE LA COPIE","COPY NAME"},
        {"A  Nom du thème.ogg","A  Theme name.ogg"}, {"X  BGM.ogg","X  BGM.ogg"},
        {"B  Retour au choix du dossier","B  Back to folder selection"}, {"FICHIER :","FILE:"},
        {"DOSSIER :","FOLDER:"}, {"AUDIO COPIÉ","AUDIO COPIED"}, {"AUCUNE COPIE","NOT COPIED"},
        {"Destination :","Destination:"}, {"Le résultat reste affiché jusqu'à validation.","This result stays until you confirm."},
        {"A ou B : retour à la liste","A or B: return to list"}, {"GUIDE RAPIDE","QUICK GUIDE"},
        {"COMMANDES","CONTROLS"}, {"A  Valider / continuer / copier","A  Confirm / continue / copy"},
        {"B  Retour / annuler","B  Back / cancel"}, {"X  Choisir le dossier sélectionné","X  Choose selected folder"},
        {"SELECT au démarrage : revoir le guide","Hold SELECT at startup to replay the guide"},
        {"A  OK / suivant       B  retour","A  OK / next       B  back"},
        {"PRÉPARATION DE LA BIBLIOTHÈQUE","PREPARING MUSIC LIBRARY"},
        {"Recherche des BGM.ogg sur la carte SD","Searching for BGM.ogg on SD card"},
        {"Les archives ZIP sont vérifiées sans extraction.","ZIP archives are checked without extraction."},
        {"CHARGEMENT","LOADING"}, {"Analyse de SD:/themes et de ses dossiers","Scanning SD:/themes and its folders"},
        {"La liste s'affichera dès que l'analyse sera terminée.","The list appears when scanning is complete."},
        {"Dossier SD:/themes introuvable : créé.","SD:/themes folder was missing and has been created."},
        {"Aucun BGM.ogg trouvé dans SD:/themes.","No BGM.ogg found in SD:/themes."}
    };
    for (const auto &entry : entries)
        if (s == entry.fr) return entry.en;
    return s;
}
static void drawText(const std::string &s,float x,float y,float scale,u32 color) {
    const std::string translated = translateUiText(s);
    C2D_Text t;
    float renderScale = topTextContext ? scale * 1.14f :
                        (bottomTextContext ? scale * 1.20f : scale);
    const char *parsed = nullptr;
    if (uiFont) {
        // The embedded 40 px font was previously reduced to 40% of the requested
        // size, leaving small labels at only a few screen pixels. Draw it at the
        // intended UI scale and keep a 10 px floor for compact helper labels.
        if (renderScale < 0.25f) renderScale = 0.25f;
        parsed = C2D_TextFontParse(&t,uiFont,textBuf,translated.c_str());
    } else {
        parsed = C2D_TextParse(&t,textBuf,translated.c_str());
    }
    if (parsed) {
        if (topTextContext || bottomTextContext) {
            float width = 0.0f, height = 0.0f;
            C2D_TextGetDimensions(&t,renderScale,renderScale,&width,&height);
            const float maxWidth = (topTextContext ? 379.0f : 309.0f) - x;
            const float maxHeight = (topTextContext ? 228.0f : 236.0f) - y;
            float fit = 1.0f;
            if (maxWidth > 0.0f && width > maxWidth) fit = maxWidth / width;
            if (maxHeight > 0.0f && height > maxHeight && maxHeight / height < fit)
                fit = maxHeight / height;
            if (fit < 1.0f) renderScale *= fit;
        }
        C2D_DrawText(&t,C2D_WithColor,x,y,0.5f,renderScale,renderScale,color);
    }
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
static size_t previewMemoryRead(void *ptr,size_t size,size_t nmemb,void *datasource) {
    PreviewMemorySource *source=(PreviewMemorySource*)datasource;
    if(!source || !source->data || !size || !nmemb || nmemb>(size_t)-1/size) return 0;
    size_t requested=size*nmemb;
    size_t remaining=source->size-source->offset;
    size_t bytes=requested<remaining?requested:remaining;
    memcpy(ptr,source->data+source->offset,bytes);
    source->offset+=bytes;
    return bytes/size;
}
static int previewMemorySeek(void *datasource,ogg_int64_t offset,int whence) {
    PreviewMemorySource *source=(PreviewMemorySource*)datasource;
    if(!source || !source->data) return -1;
    ogg_int64_t base;
    if(whence==SEEK_SET) base=0;
    else if(whence==SEEK_CUR) base=(ogg_int64_t)source->offset;
    else if(whence==SEEK_END) base=(ogg_int64_t)source->size;
    else return -1;
    if(offset < -base || offset > (ogg_int64_t)source->size-base) return -1;
    source->offset=(size_t)(base+offset);
    return 0;
}
static int previewMemoryClose(void *datasource) { (void)datasource; return 0; }
static long previewMemoryTell(void *datasource) {
    PreviewMemorySource *source=(PreviewMemorySource*)datasource;
    return source?(long)source->offset:-1;
}
static ov_callbacks previewMemoryCallbacks={previewMemoryRead,previewMemorySeek,previewMemoryClose,previewMemoryTell};
static void stopPreview();
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
static bool loadZipEntryForPreview(const std::string &zip,const std::string &entry) {
    if(previewMemorySource.data) free(previewMemorySource.data);
    previewMemorySource={nullptr,0,0};
    struct archive *a=archive_read_new(); if(!a) return false;
    archive_read_support_filter_none(a); archive_read_support_format_zip(a);
    bool opened=archive_read_open_filename(a,zip.c_str(),16384)==ARCHIVE_OK;
    bool loaded=false;
    if(opened) {
        struct archive_entry *e=nullptr;
        while(archive_read_next_header(a,&e)==ARCHIVE_OK) {
            const char *p=archive_entry_pathname(e);
            if(p && entry==p) {
                la_int64_t length=archive_entry_size(e);
                if(length<=0 || (u64)length>MAX_PREVIEW_RAM_BYTES) break;
                unsigned char *data=(unsigned char*)malloc((size_t)length);
                if(!data) break;
                size_t offset=0;
                while(offset<(size_t)length) {
                    la_ssize_t n=archive_read_data(a,data+offset,(size_t)length-offset);
                    if(n<=0) break;
                    offset+=(size_t)n;
                }
                if(offset==(size_t)length) {
                    previewMemorySource.data=data;
                    previewMemorySource.size=(size_t)length;
                    previewMemorySource.offset=0;
                    loaded=true;
                } else free(data);
                break;
            }
            archive_read_data_skip(a);
        }
        archive_read_close(a);
    }
    archive_read_free(a);
    return loaded;
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
                    const size_t bufferSize=16*1024;
                    char *buf=(char*)malloc(bufferSize);
                    if(buf) {
                        la_ssize_t n; ok=true;
                        while((n=archive_read_data(a,buf,bufferSize))>0) if(fwrite(buf,1,(size_t)n,out)!=(size_t)n) { ok=false; break; }
                        if(n<0) ok=false;
                        free(buf);
                    }
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
static void scanThemeFolder(const std::string &folder,int depth,std::vector<Track> &found) {
    if(depth>MAX_THEME_DEPTH || found.size()>=MAX_TRACKS) return;
    DIR *d=opendir(folder.c_str()); if(!d) return;
    struct dirent *it;
    while((it=readdir(d)) && found.size()<MAX_TRACKS) {
        if(it->d_name[0]=='.') continue;
        scanEntriesVisited++;
        std::string p=joinPath(folder,it->d_name);
        if(isDir(p)) {
            std::string bgm=joinPath(p,"BGM.ogg"); struct stat st;
            if(stat(bgm.c_str(),&st)==0 && S_ISREG(st.st_mode)) found.push_back({themeTitle(it->d_name),bgm,"",(u64)st.st_size,false});
            scanThemeFolder(p,depth+1,found);
        } else if(endsWithI(it->d_name,".zip")) {
            std::string entry; u64 fileSize=0;
            if(zipFindBgm(p,entry,fileSize)) found.push_back({themeTitle(it->d_name),p,entry,fileSize,true});
        } else if(strcasecmp(it->d_name,"BGM.ogg")==0 && depth==0) {
            struct stat st; u64 fileSize=(stat(p.c_str(),&st)==0 && S_ISREG(st.st_mode))?(u64)st.st_size:0;
            found.push_back({"Thème sans nom",p,"",fileSize,false});
        }
    }
    closedir(d);
}
static void scanWorker(void *) {
    std::vector<Track> found;
    DIR *d=opendir(THEMES_DIR);
    scanRootMissing=(d==nullptr);
    if(d) {
        closedir(d);
        scanThemeFolder(THEMES_DIR,0,found);
    } else {
        mkdir(THEMES_DIR,0777);
    }
    std::sort(found.begin(),found.end(),[](const Track&a,const Track&b){return strcasecmp(a.title.c_str(),b.title.c_str())<0;});
    scanResults.swap(found);
    scanFinished=true;
}
static void beginThemeScan(bool guideAfter=false) {
    if(scanWorkerThread) return;
    stopPreview();
    scanResults.clear(); scanEntriesVisited=0; scanRootMissing=false; scanFinished=false;
    showGuideAfterScan=guideAfter; loadingVisible=true; scanStartedAt=osGetTime();
    scanWorkerThread=threadCreate(scanWorker,NULL,64*1024,0x3F,-2,false);
    if(!scanWorkerThread) {
        scanWorker(NULL);
    }
}
static void finishThemeScan() {
    if(!loadingVisible || !scanFinished || osGetTime()-scanStartedAt<450) return;
    if(scanWorkerThread) {
        threadJoin(scanWorkerThread,U64_MAX);
        threadFree(scanWorkerThread);
        scanWorkerThread=nullptr;
    }
    tracks.swap(scanResults); selected=0; firstVisible=0; loadingVisible=false;
    if(scanRootMissing) setToast("Dossier SD:/themes introuvable : créé.",3600);
    else if(tracks.empty()) setToast("Aucun BGM.ogg trouvé dans SD:/themes.",3600);
    else setToast((std::to_string(tracks.size())+" thème(s) musical(aux) trouvé(s).").c_str());
    if(languagePromptAfterScan) {
        languagePromptVisible=true;
        languagePromptAfterScan=false;
    } else if(showGuideAfterScan) {
        guideVisible=true; guidePage=0; showGuideAfterScan=false;
    }
}

static void stopPreview() {
    previewStopRequested=true;
    if(previewWorkerThread) {
        threadJoin(previewWorkerThread,U64_MAX);
        threadFree(previewWorkerThread);
        previewWorkerThread=nullptr;
    }
    if(ndspInitialized) { ndspChnWaveBufClear(0); ndspChnReset(0); }
    memset(audioWaves,0,sizeof(audioWaves));
    if(oggOpen) { ov_clear(&oggFile); oggOpen=false; }
    if(previewMemorySource.data) free(previewMemorySource.data);
    previewMemorySource={nullptr,0,0};
    previewing=false; previewEof=false; previewDecodeError=false;
    if(previewFromTemp) { remove(PREVIEW_TEMP); previewFromTemp=false; }
}

static size_t decodePreviewBuffer(s16 *output, bool *reachedEof) {
    const size_t bytesPerFrame=(size_t)previewChannels*sizeof(s16);
    const size_t target=AUDIO_SAMPLES*bytesPerFrame;
    size_t used=0;
    unsigned errors=0;
    while(used<target && !previewStopRequested) {
        int bitstream=0;
        long bytes=ov_read(&oggFile,(char*)output+used,(int)(target-used),&bitstream);
        if(bytes>0) {
            vorbis_info *info=ov_info(&oggFile,bitstream);
            if(!info || info->channels!=previewChannels || info->rate!=previewRate) {
                previewDecodeError=true;
                break;
            }
            used+=(size_t)bytes;
            errors=0;
        } else if(bytes==0) {
            *reachedEof=true;
            break;
        } else if(++errors>5) {
            previewDecodeError=true;
            break;
        }
    }
    used-=used%bytesPerFrame;
    return used;
}

static void previewWorker(void *unused) {
    (void)unused;
    while(!previewStopRequested) {
        bool filled=false;
        if(!previewEof && !previewDecodeError) {
            for(int i=0;i<AUDIO_BUFFERS && !previewStopRequested && !previewEof && !previewDecodeError;i++) {
                if(audioWaves[i].status!=NDSP_WBUF_DONE && audioWaves[i].status!=NDSP_WBUF_FREE) continue;
                bool reachedEof=false;
                size_t bytes=decodePreviewBuffer(audioMemory[i],&reachedEof);
                if(bytes) {
                    DSP_FlushDataCache(audioMemory[i],(u32)bytes);
                    memset(&audioWaves[i],0,sizeof(audioWaves[i]));
                    audioWaves[i].data_vaddr=audioMemory[i];
                    audioWaves[i].nsamples=(u32)(bytes/((size_t)previewChannels*sizeof(s16)));
                    ndspChnWaveBufAdd(0,&audioWaves[i]);
                    filled=true;
                    if(reachedEof) previewEof=true;
                    break;
                }
                if(reachedEof) previewEof=true;
            }
        }
        svcSleepThread(filled?1000000LL:5000000LL);
    }
}

static bool startPreview(const Track &t) {
    stopPreview();
    if(!audioReady) { setToast("Audio de la 3DS indisponible."); return false; }
    bool opened=false;
    previewFromTemp=false;
    if(t.zipped && loadZipEntryForPreview(t.source,t.entry)) {
        if(ov_open_callbacks(&previewMemorySource,&oggFile,nullptr,0,previewMemoryCallbacks)>=0) opened=true;
        else {
            free(previewMemorySource.data);
            previewMemorySource={nullptr,0,0};
            setToast("Impossible d'ouvrir ce BGM.ogg."); return false;
        }
    } else {
        bool ready=t.zipped?extractZipEntry(t.source,t.entry,PREVIEW_TEMP):true;
        const char *path=t.zipped?PREVIEW_TEMP:t.source.c_str();
        FILE *audioFile=ready?fopen(path,"rb"):nullptr;
        if(audioFile && ov_open(audioFile,&oggFile,nullptr,0)>=0) {
            opened=true; previewFromTemp=t.zipped;
        } else {
            if(audioFile) fclose(audioFile);
            if(t.zipped) remove(PREVIEW_TEMP);
            setToast("Impossible d'ouvrir ce BGM.ogg."); return false;
        }
    }
    if(!opened) { setToast("Impossible d'ouvrir ce BGM.ogg."); return false; }
    oggOpen=true;
    vorbis_info *info=ov_info(&oggFile,0);
    if(!info || (info->channels!=1 && info->channels!=2) || info->rate<8000 || info->rate>96000) {
        stopPreview(); setToast("Format OGG non pris en charge."); return false;
    }
    previewChannels=info->channels; previewRate=info->rate;
    ndspChnReset(0); ndspChnSetInterp(0,NDSP_INTERP_LINEAR);
    ndspChnSetRate(0,(float)info->rate);
    ndspChnSetFormat(0,previewChannels==1?NDSP_FORMAT_MONO_PCM16:NDSP_FORMAT_STEREO_PCM16);
    float mix[12]={1.0f,1.0f,0,0,0,0,0,0,0,0,0,0}; ndspChnSetMix(0,mix);
    previewStopRequested=false; previewEof=false; previewDecodeError=false; previewing=true;
    previewWorkerThread=threadCreate(previewWorker,NULL,16*1024,0x28,-2,false);
    if(!previewWorkerThread) {
        stopPreview(); setToast("Impossible de démarrer le décodage audio."); return false;
    }
    setToast("Préécoute en cours. Y ou B pour arrêter."); return true;
}
static void pumpPreview() {
    if(!previewing || !oggOpen) return;
    if(previewDecodeError) {
        stopPreview(); setToast("Erreur de décodage OGG ou flux chaîné incompatible."); return;
    }
    if(previewEof) {
        bool queued=false;
        for(int i=0;i<AUDIO_BUFFERS;i++) if(audioWaves[i].status==NDSP_WBUF_QUEUED || audioWaves[i].status==NDSP_WBUF_PLAYING) queued=true;
        if(!queued) { stopPreview(); setToast("Fin de l'écoute."); }
    }
}

static bool copyFileTo(const std::string &src,const std::string &dest) {
    const size_t bufferSize=32*1024;
    char *buf=(char*)malloc(bufferSize);
    if(!buf) return false;
    FILE *in=fopen(src.c_str(),"rb"), *out=fopen(dest.c_str(),"wb");
    if(!in || !out) { if(in) fclose(in); if(out) fclose(out); free(buf); return false; }
    size_t n; bool ok=true;
    while((n=fread(buf,1,bufferSize,in))>0) if(fwrite(buf,1,n,out)!=n) {ok=false;break;}
    if(ferror(in)) ok=false;
    fclose(in);
    if(fclose(out)!=0) ok=false;
    free(buf);
    if(!ok) remove(dest.c_str());
    return ok;
}
static bool copyZipEntryTo(const Track &t,const std::string &dest) {
    // Keep large I/O buffers off the small main-thread stack on Old 3DS.
    const size_t bufferSize=32*1024;
    char *buf=(char*)malloc(bufferSize);
    if(!buf) return false;
    struct archive *a=archive_read_new(); if(!a) { free(buf); return false; }
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
                    la_ssize_t n; ok=true;
                    while((n=archive_read_data(a,buf,bufferSize))>0) if(fwrite(buf,1,(size_t)n,out)!=(size_t)n) {ok=false;break;}
                    if(n<0) ok=false;
                    if(fclose(out)!=0) ok=false;
                }
                break;
            }
            archive_read_data_skip(a);
        }
        archive_read_close(a);
    }
    archive_read_free(a); free(buf); if(!ok) remove(dest.c_str()); return ok;
}
static std::string safeFilename(std::string s) {
    for(char &c:s) if((unsigned char)c<32 || strchr("\\/:*?\"<>|",c)) c='_';
    while(!s.empty() && (s.back()=='.' || s.back()==' ')) s.pop_back();
    if(s.empty()) s="Theme audio";
    // Leave room for ".ogg" and stay below FAT's per-component filename limit.
    if(s.size()>200) {
        size_t cut=200;
        while(cut>0 && ((unsigned char)s[cut]&0xC0)==0x80) cut--;
        s.resize(cut);
        while(!s.empty() && (s.back()=='.' || s.back()==' ')) s.pop_back();
    }
    return s;
}
static bool copySelectedTo(const std::string &outputPath) {
    if(selected>=tracks.size()) return false;
    const Track &t=tracks[selected];
    // Opening the same source and destination with "wb" would truncate a
    // folder-based BGM.ogg before it can be copied.
    if(!t.zipped && strcasecmp(t.source.c_str(),outputPath.c_str())==0) return false;
    return t.zipped?copyZipEntryTo(t,outputPath):copyFileTo(t.source,outputPath);
}

static bool folderHasMp3(const std::string &path) {
    DIR *d=opendir(path.c_str()); if(!d) return false;
    struct dirent *it; bool found=false; unsigned examined=0;
    while((it=readdir(d)) && examined++<256) if(endsWithI(it->d_name,".mp3")) { found=true; break; }
    closedir(d); return found;
}
static void readDestDirs() {
    destDirs.clear(); DIR *d=opendir(destPath.c_str());
    if(!d) { setToast("Impossible d'ouvrir ce dossier SD."); return; }
    struct dirent *it; unsigned examined=0; bool truncated=false;
    while((it=readdir(d))) {
        if(examined++>=512) { truncated=true; break; }
        if(it->d_name[0]=='.') continue;
        std::string p=joinPath(destPath,it->d_name);
        if(p.size()>MAX_SD_PATH_BYTES) continue;
        if(isDir(p)) {
            if(destDirs.size()>=MAX_DEST_DIRS) { truncated=true; continue; }
            destDirs.push_back({it->d_name,p,folderHasMp3(p)});
        }
    }
    closedir(d);
    std::sort(destDirs.begin(),destDirs.end(),[](const DestDir&a,const DestDir&b){if(a.hasMp3!=b.hasMp3)return a.hasMp3>b.hasMp3;return strcasecmp(a.name.c_str(),b.name.c_str())<0;});
    destSelected=0;
    if(truncated) setToast("Liste limitée pour accélérer l'exploration.");
}
static std::string parentPath(const std::string &path) {
    if(path=="sdmc:/" || path=="sdmc:") return "sdmc:/";
    size_t end=path.size(); while(end>6 && path[end-1]=='/') end--;
    size_t slash=path.find_last_of('/',end-1);
    if(slash==std::string::npos || slash<5) return "sdmc:/";
    return path.substr(0,slash);
}
static void enterDestination() { selectingDestination=true; choosingFilename=false; confirmOverwrite=false; firstVisible=0; destPath="sdmc:/"; readDestDirs(); }
static void chooseDestination() {
    if(!isDir(destPath)) { setToast("Ce dossier n'est plus accessible."); return; }
    selectingDestination=false; choosingFilename=true;
}
static void showCopyResult(CopyResultState result,const char *detail) {
    copyResult=result;
    copyResultDetail=detail?detail:"";
    selectingDestination=false;
    choosingFilename=false;
    confirmOverwrite=false;
    toast.clear(); toastUntil=0;
}
static void beginCopy(bool useThemeName) {
    if(selected>=tracks.size()) return;
    std::string filename=useThemeName?safeFilename(tracks[selected].title)+".ogg":"BGM.ogg";
    pendingOutputPath=joinPath(destPath,filename);
    if(filename.size()>255 || pendingOutputPath.size()>MAX_SD_PATH_BYTES) {
        showCopyResult(COPY_RESULT_FAILED,"Nom ou chemin trop long pour la carte SD."); return;
    }
    if(!tracks[selected].zipped && strcasecmp(tracks[selected].source.c_str(),pendingOutputPath.c_str())==0) {
        showCopyResult(COPY_RESULT_CANCELLED,"La source est déjà dans ce dossier. Rien n'a été copié.");
        return;
    }
    if(access(pendingOutputPath.c_str(),F_OK)==0) { confirmOverwrite=true; choosingFilename=false; return; }
    stopPreview();
    if(copySelectedTo(pendingOutputPath)) showCopyResult(COPY_RESULT_SUCCESS,"Le fichier est prêt dans le dossier indiqué.");
    else showCopyResult(COPY_RESULT_FAILED,"Vérifie la carte SD, l'espace libre et les droits d'accès.");
}
static void overwriteCopy() {
    stopPreview();
    if(copySelectedTo(pendingOutputPath)) showCopyResult(COPY_RESULT_SUCCESS,"Le fichier a bien été remplacé dans le dossier indiqué.");
    else showCopyResult(COPY_RESULT_FAILED,"Vérifie la carte SD, l'espace libre et les droits d'accès.");
}

static void drawHeader(const char *title,const char *right) {
    drawText("SHIELD",22,7,0.48f,C_WHITE);
    drawText(title,100,7,0.48f,C_CYAN);
    drawText(right,103,27,0.30f,C_WHITE);
    rect(20,46,360,1,C_CYAN);
}
static void drawTopScreen() {
    topTextContext = true;
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
        drawText("DEPUIS :",105,101,0.30f,C_CYAN); drawText(fitText(t.title,27),150,99,0.35f,C_WHITE);
        drawText("Format :",105,123,0.30f,C_CYAN); drawText(t.zipped?"ZIP":"Dossier",150,121,0.34f,C_WHITE);
        char sizeText[32];
        if(t.size >= 1024u*1024u) snprintf(sizeText,sizeof(sizeText),"%.1f MB",(double)t.size/(1024.0*1024.0));
        else snprintf(sizeText,sizeof(sizeText),"%llu KB",(unsigned long long)((t.size+1023u)/1024u));
        drawText("Taille :",105,145,0.30f,C_CYAN); drawText(sizeText,150,143,0.34f,C_WHITE);
        drawText("État :",105,167,0.30f,C_CYAN);
        drawText(previewing?"En lecture":"Prête à copier",150,165,0.34f,previewing?C_GREEN:C_WHITE);
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
    topTextContext = false;
}
static void drawMainBottom() {
    bottomTextContext = true;
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
        drawText(tracks[i].zipped?"ZIP":"DOS",258,y+5,0.23f,i==selected?C_BG:C_MUTED);
    }
    rect(17,172,136,30,C_PANEL2); rect(17,172,136,1,C_CYAN); rect(17,201,136,1,C_CYAN);
    rect(17,172,1,30,C_CYAN); rect(152,172,1,30,C_CYAN); rect(30,180,10,12,C_CYAN); rect(32,178,10,12,C_CYAN); drawText("COPIER",57,180,0.31f,C_WHITE);
    rect(167,172,136,30,C_PANEL2); rect(167,172,136,1,C_CYAN); rect(167,201,136,1,C_CYAN);
    rect(167,172,1,30,C_CYAN); rect(302,172,1,30,C_CYAN); rect(185,181,18,11,C_CYAN); rect(188,178,12,3,C_CYAN); drawText("DOSSIER",213,180,0.31f,C_WHITE);
    rect(10,211,300,1,C_EDGE);
    drawText("A : Choisir     Y : Écouter     B : Retour",16,218,0.24f,C_WHITE);
    bottomTextContext = false;
}
static void drawDestBottom() {
    bottomTextContext = true;
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    drawText("DOSSIERS DE DESTINATION",12,8,0.34f,C_WHITE);
    rect(10,31,300,1,C_EDGE);
    bool currentSelected=destSelected==0;
    rect(10,36,300,29,currentSelected?C_CYAN:C_PANEL2);
    rect(10,36,currentSelected?3:1,29,currentSelected?C_CYAN:C_EDGE);
    std::string currentLabel=destPath=="sdmc:/"?"DOSSIER ACTUEL : CARTE SD":"DOSSIER ACTUEL : "+leafName(destPath);
    drawText(fitText(currentLabel,31),20,41,0.31f,currentSelected?C_BG:C_WHITE);
    const int rows=4;
    for(int row=0;row<rows;row++) {
        size_t i=firstVisible+(size_t)row; if(i>=destDirs.size()) break;
        float y=69+row*31.0f;
        bool isSelected=destSelected==i+1;
        rect(10,y,300,27,isSelected?C_PANEL2:C_PANEL);
        rect(10,y,isSelected?3:1,27,isSelected?C_CYAN:C_EDGE);
        drawText(fitText(destDirs[i].name,25),20,y+4,0.32f,isSelected?C_WHITE:C_MUTED);
        if(destDirs[i].hasMp3) drawText("MP3",267,y+6,0.25f,C_GOLD);
    }
    rect(10,211,300,1,C_EDGE);
    drawText("A : ouvrir   X : choisir   B : retour",12,217,0.25f,C_MUTED);
    bottomTextContext = false;
}
static void drawCopyBottom() {
    bottomTextContext = true;
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
    bottomTextContext = false;
}
static void drawCopyResult() {
    const bool success=copyResult==COPY_RESULT_SUCCESS;
    const bool cancelled=copyResult==COPY_RESULT_CANCELLED;
    const u32 accent=success?C_GREEN:(cancelled?C_GOLD:C_RED);
    const char *title=success?"COPIE RÉUSSIE":(cancelled?"COPIE ANNULÉE":"COPIE ÉCHOUÉE");

    topTextContext = true;
    C2D_TargetClear(topTarget,C_BG); C2D_SceneBegin(topTarget);
    drawHeader("RÉSULTAT DE COPIE",success?"OK":"À VÉRIFIER");
    rect(16,52,368,174,C_PANEL); rect(16,52,2,174,accent); rect(382,52,2,174,accent);
    rect(28,65,344,43,accent);
    drawText(title,42,73,0.55f,C_BG);
    drawText("FICHIER :",31,121,0.31f,C_CYAN);
    drawText(fitText(leafName(pendingOutputPath),37),120,119,0.32f,C_WHITE);
    drawText("DOSSIER :",31,148,0.31f,C_CYAN);
    drawText(fitText(destPath,42),120,146,0.30f,C_WHITE);
    drawText(fitText(copyResultDetail,55),31,184,0.29f,success?C_GREEN:(cancelled?C_GOLD:C_WHITE));

    topTextContext = false;
    bottomTextContext = true;
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    rect(10,22,300,190,C_PANEL); rect(10,22,300,3,accent);
    drawText(success?"AUDIO COPIÉ":"AUCUNE COPIE",24,39,0.45f,accent);
    drawText(fitText(leafName(pendingOutputPath),31),24,78,0.33f,C_WHITE);
    drawText("Destination :",24,111,0.30f,C_CYAN);
    drawText(fitText(destPath,34),24,134,0.30f,C_WHITE);
    drawText("Le résultat reste affiché jusqu'à validation.",24,171,0.27f,C_MUTED);
    rect(10,221,300,1,C_EDGE);
    drawText("A ou B : retour à la liste",31,225,0.29f,C_CYAN);
    bottomTextContext = false;
}
static void drawGuide() {
    topTextContext = true;
    C2D_TargetClear(topTarget,C_BG); C2D_SceneBegin(topTarget);
    drawHeader("GUIDE RAPIDE","3 PAGES");
    rect(22,58,356,150,C_PANEL); rect(22,58,4,150,C_CYAN);
    static const char *titlesFr[]={"TROUVE TES MUSIQUES","ECOUTE AVANT DE COPIER","CHOISIS TA DESTINATION"};
    static const char *titlesEn[]={"FIND YOUR MUSIC","PREVIEW BEFORE COPYING","CHOOSE A DESTINATION"};
    static const char *linesFr[3][3]={
        {"Recherche SD:/themes et sous-dossiers", "archives ZIP ou dossiers extraits", "compatibles avec Anemone."},
        {"Choisis un theme dans la liste.", "Y lance ou arrete l'ecoute", "de l'aper?u BGM.ogg."},
        {"Haut/Bas choisit un dossier.", "A l'ouvre; X le selectionne.", "Les dossiers avec MP3 passent avant."}
    };
    static const char *linesEn[3][3]={
        {"Themes are searched in SD:/themes", "and subfolders, as ZIP archives", "or extracted Anemone folders."},
        {"Select a theme from the list.", "Press Y to start or stop the", "BGM.ogg preview."},
        {"Up/Down selects a folder.", "A opens it; X selects it.", "Folders with MP3 files come first."}
    };
    const char *const *titles=uiLanguage.french?titlesFr:titlesEn;
    const char *const (*lines)[3]=uiLanguage.french?linesFr:linesEn;
    drawText(titles[guidePage],39,78,0.43f,C_CYAN);
    for(int i=0;i<3;i++) drawText(lines[guidePage][i],39,111+i*22,0.38f,C_WHITE);
    char page[24]; snprintf(page,sizeof(page),uiLanguage.french?"PAGE %d / 3":"PAGE %d / 3",guidePage+1);
    drawText(page,39,182,0.28f,C_MUTED);
    topTextContext = false;
    bottomTextContext = true;
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    rect(10,18,300,195,C_PANEL);
    drawText(uiLanguage.french?"COMMANDES":"CONTROLS",24,32,0.47f,C_WHITE);
    drawText(uiLanguage.french?"A  Valider / continuer / copier":"A  Confirm / continue / copy",24,72,0.36f,C_GREEN);
    drawText(uiLanguage.french?"B  Retour / annuler":"B  Back / cancel",24,105,0.38f,C_WHITE);
    if(guidePage==2) drawText(uiLanguage.french?"X  Choisir le dossier selectionne":"X  Choose selected folder",24,130,0.33f,C_CYAN);
    drawText(uiLanguage.french?"SELECT au demarrage : revoir le guide":"Hold SELECT at startup to replay guide",24,173,0.31f,C_MUTED);
    drawText(uiLanguage.french?"A  OK / suivant       B  retour":"A  OK / next       B  back",20,222,0.31f,C_CYAN);
    bottomTextContext = false;
}
static void drawLanguagePrompt() {
    topTextContext = true;
    C2D_TargetClear(topTarget,C_BG); C2D_SceneBegin(topTarget);
    drawHeader(uiLanguage.french?"CHOIX DE LANGUE":"LANGUAGE",uiLanguage.french?"PREMIER LANCEMENT":"FIRST START");
    rect(22,58,356,150,C_PANEL); rect(22,58,4,150,C_CYAN);
    drawText(uiLanguage.french?"La langue detectee est le francais ?":"Is English your preferred language?",38,82,0.42f,C_WHITE);
    drawText(uiLanguage.french?"A : Oui":"A: Yes",38,120,0.42f,C_GREEN);
    drawText(uiLanguage.french?"B : Changer la langue":"B: Change language",38,153,0.38f,C_CYAN);
    drawText(uiLanguage.french?"Langues disponibles : francais et English.":"Available languages: English and French.",38,181,0.28f,C_MUTED);
    topTextContext = false;
    bottomTextContext = true;
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    rect(10,18,300,195,C_PANEL);
    drawText(uiLanguage.french?"CHOIX INITIAL":"FIRST START",24,38,0.45f,C_WHITE);
    drawText(uiLanguage.french?"L'application ne change aucun reglage":"The app does not change system settings",24,83,0.30f,C_MUTED);
    drawText(uiLanguage.french?"de la console.":"on your console.",24,103,0.30f,C_MUTED);
    drawText(uiLanguage.french?"Choisis entre francais et English.":"Choose English or French.",24,137,0.30f,C_CYAN);
    drawText(uiLanguage.french?"A : confirmer   B : changer":"A: confirm   B: change",24,171,0.32f,C_WHITE);
    drawText(uiLanguage.french?"L'anglais peut contenir des erreurs.":"My English may contain mistakes.",24,193,0.27f,C_MUTED);
    bottomTextContext = false;
}
static void drawLoading() {
    topTextContext = true;
    C2D_TargetClear(topTarget,C_BG); C2D_SceneBegin(topTarget);
    drawHeader("THEME MUSIC","DÉMARRAGE");
    rect(26,67,348,126,C_PANEL); rect(26,67,3,126,C_CYAN);
    musicNote(53,104,2.0f,C_CYAN);
    drawText("PRÉPARATION DE LA BIBLIOTHÈQUE",105,91,0.37f,C_CYAN);
    drawText("Recherche des BGM.ogg sur la carte SD",105,119,0.36f,C_WHITE);
    drawText("Les archives ZIP sont vérifiées sans extraction.",105,148,0.29f,C_MUTED);
    unsigned dots=(unsigned)((osGetTime()/350)%4);
    for(unsigned i=0;i<4;i++) circle(165+i*18,181,3.2f,i<=dots?C_CYAN:C_EDGE);

    topTextContext = false;
    bottomTextContext = true;
    C2D_TargetClear(bottomTarget,C_BG); C2D_SceneBegin(bottomTarget);
    rect(12,22,296,194,C_PANEL); rect(12,22,296,2,C_CYAN);
    drawText("CHARGEMENT",28,41,0.48f,C_WHITE);
    drawText("Analyse de SD:/themes et de ses dossiers",28,82,0.34f,C_MUTED);
    char count[48]; snprintf(count,sizeof(count),"Éléments examinés : %lu",(unsigned long)scanEntriesVisited);
    drawText(count,28,111,0.36f,C_CYAN);
    rect(28,151,264,8,C_EDGE);
    unsigned phase=(unsigned)((osGetTime()/80)%24);
    rect(28+(phase<12?phase:23-phase)*11,151,44,8,C_CYAN);
    drawText("La liste s'affichera dès que l'analyse sera terminée.",28,182,0.28f,C_WHITE);
    bottomTextContext = false;
}
static void drawAll() {
    topTextContext = false;
    bottomTextContext = false;
    C2D_TextBufClear(textBuf);
    if(loadingVisible) drawLoading();
    else if(languagePromptVisible) drawLanguagePrompt();
    else if(guideVisible) drawGuide();
    else if(copyResult!=COPY_RESULT_NONE) drawCopyResult();
    else { drawTopScreen(); if(choosingFilename || confirmOverwrite) drawCopyBottom(); else if(selectingDestination) drawDestBottom(); else drawMainBottom(); }
}

static void finishGuide() {
    guideVisible=false; guideSeen=true; FILE *f=fopen(GUIDE_MARKER,"wb"); if(f){fputc(1,f);fclose(f);}
}
static void handleGuide(u32 keys) {
    if(keys&KEY_A) { if(guidePage<2) guidePage++; else finishGuide(); }
    if(keys&KEY_B) { if(guidePage>0) guidePage--; else finishGuide(); }
}
static void handleLanguagePrompt(u32 keys) {
    if(keys&KEY_B) shield_language_choice_toggle(&uiLanguage);
    if(keys&KEY_A) {
        shield_language_choice_accept(&uiLanguage);
        languagePromptVisible=false;
        if(showGuideAfterScan) { guideVisible=true; guidePage=0; showGuideAfterScan=false; }
    }
}
static void handleMain(u32 keys) {
    if(copyResult!=COPY_RESULT_NONE) {
        if(keys&(KEY_A|KEY_B)) copyResult=COPY_RESULT_NONE;
        return;
    }
    if(confirmOverwrite) {
        if(keys&KEY_A) overwriteCopy();
        else if(keys&KEY_B) { confirmOverwrite=false; choosingFilename=true; }
        return;
    }
    if(choosingFilename) {
        if(keys&KEY_A) beginCopy(true);
        else if(keys&KEY_X) beginCopy(false);
        else if(keys&KEY_B) { choosingFilename=false; selectingDestination=true; }
        return;
    }
    if(selectingDestination) {
        if(keys&KEY_B) {
            if(destPath=="sdmc:/") selectingDestination=false;
            else { destPath=parentPath(destPath); readDestDirs(); firstVisible=0; }
            return;
        }
        if(keys&KEY_UP) {
            if(destSelected>0) destSelected--;
            if(destSelected>0 && destSelected-1<firstVisible) firstVisible=destSelected-1;
            return;
        }
        if(keys&KEY_DOWN) {
            if(destSelected<destDirs.size()) destSelected++;
            if(destSelected>0 && destSelected-1>=firstVisible+4) firstVisible=destSelected-4;
            return;
        }
        if(destSelected>0 && destSelected-1<firstVisible) firstVisible=destSelected-1;
        if(destSelected>0 && destSelected-1>=firstVisible+4) firstVisible=destSelected-4;
        if((keys&KEY_A) && destSelected>0 && destSelected-1<destDirs.size()) {
            destPath=destDirs[destSelected-1].path; readDestDirs(); firstVisible=0; return;
        }
        if(keys&KEY_X) {
            if(destSelected>0 && destSelected-1<destDirs.size()) destPath=destDirs[destSelected-1].path;
            chooseDestination(); return;
        }
        return;
    }
    if(keys&KEY_X) { beginThemeScan(false); return; }
    if(keys&KEY_UP && selected>0) { selected--; if(previewing) stopPreview(); }
    if(keys&KEY_DOWN && selected+1<tracks.size()) { selected++; if(previewing) stopPreview(); }
    if(keys&KEY_B) stopPreview();
    if((keys&KEY_Y) && selected<tracks.size()) { if(previewing) stopPreview(); else startPreview(tracks[selected]); }
    if((keys&KEY_A) && selected<tracks.size()) enterDestination();
}
int main() {
    gfxInitDefault();
    hidScanInput();
    const bool forceLanguageSelection=(hidKeysHeld()&KEY_X)!=0;
    const bool review=(hidKeysHeld()&KEY_SELECT)!=0;
    // Citro2D renders to both framebuffers. consoleInit(GFX_BOTTOM, ...) changes
    // the bottom screen's pixel format for the debug console and can corrupt
    // subsequent GPU output there, so keep both screens in the default BGR8 mode.
    gfxSetScreenFormat(GFX_TOP,GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM,GSP_BGR8_OES);
    if(!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) || !C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { gfxExit(); return 1; }
    C2D_Prepare(); topTarget=C2D_CreateScreenTarget(GFX_TOP,GFX_LEFT); bottomTarget=C2D_CreateScreenTarget(GFX_BOTTOM,GFX_LEFT); textBuf=C2D_TextBufNew(8192);
    uiFont=C2D_FontLoadSystem(CFG_REGION_EUR);
    if(uiFont) C2D_FontSetFilter(uiFont,GPU_LINEAR,GPU_LINEAR);
    mkdir("sdmc:/3ds",0777); mkdir(APP_DIR,0777); mkdir(THEMES_DIR,0777);
    ndspInitialized=R_SUCCEEDED(ndspInit()); audioReady=ndspInitialized; if(audioReady) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    for(int i=0;i<AUDIO_BUFFERS;i++) { audioMemory[i]=(s16*)linearAlloc(AUDIO_SAMPLES*2*sizeof(s16)); memset(&audioWaves[i],0,sizeof(audioWaves[i])); if(!audioMemory[i]) audioReady=false; }
    FILE *marker=fopen(GUIDE_MARKER,"rb"); guideSeen=marker!=nullptr; if(marker) fclose(marker);
    const bool cfguReady=R_SUCCEEDED(cfguInit());
    shield_language_choice_load(&uiLanguage,"sdmc:/3ds/ShieldThemeAudio/ui-language.dat",cfguReady);
    if(cfguReady) cfguExit();
    showGuideAfterScan=!guideSeen || review || forceLanguageSelection;
    languagePromptAfterScan=forceLanguageSelection || uiLanguage.needs_confirmation;
    beginThemeScan(showGuideAfterScan);
    while(aptMainLoop() && running) {
        hidScanInput(); u32 keys=hidKeysDown();
        if(loadingVisible) finishThemeScan();
        else if(languagePromptVisible) handleLanguagePrompt(keys);
        else if(guideVisible) handleGuide(keys);
        else { if(keys&KEY_START) running=false; else handleMain(keys); }
        pumpPreview();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW); drawAll(); C2D_Flush(); C3D_FrameEnd(0);
        gspWaitForVBlank();
    }
    stopPreview(); for(int i=0;i<AUDIO_BUFFERS;i++) if(audioMemory[i]) linearFree(audioMemory[i]);
    if(ndspInitialized) ndspExit();
    C2D_TextBufDelete(textBuf);
    if(uiFont) C2D_FontFree(uiFont);
    C2D_Fini(); C3D_Fini(); gfxExit(); return 0;
}
