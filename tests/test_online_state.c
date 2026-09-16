#include "online_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void punctuation(void) {
    const char *marks[]={"，","。","！","？","、","；","：","）","】","》","”","……","！》",",","!","?", ")", ";", ":"};
    for(unsigned m=0;m<sizeof(marks)/sizeof(marks[0]);++m) {
        for(unsigned length=1;length<=85;++length) {
            char text[400]={0},joined[400]={0},lines[12][ONLINE_LINE_BYTES];
            for(unsigned i=0;i<length;++i)strcat(text,"好");
            strcat(text,marks[m]);strcat(text,"继续加油");
            unsigned count=online_caption_lines(text,lines,12);
            for(unsigned i=0;i<count;++i) {
                assert(!strncmp(lines[i],"好",3) || !strncmp(lines[i],"继",3) ||
                       !strncmp(lines[i],"续",3) || !strncmp(lines[i],"加",3) || !strncmp(lines[i],"油",3));
                unsigned glyphs=0;
                for(const unsigned char *p=(unsigned char *)lines[i];*p;++p)
                    if((*p&0xc0)!=0x80)++glyphs;
                assert(glyphs<=14);strcat(joined,lines[i]);
            }
            assert(!strcmp(text,joined)); /* Keep punctuation; change wrapping only. */
        }
    }
    char lines[3][ONLINE_LINE_BYTES];
    assert(online_caption_lines("你好\n，今天也加油！",lines,3)==1);
    assert(!strcmp(lines[0],"你好，今天也加油！"));
}
static void captions(void) {
    char text[1801];memset(text,'a',1800);text[1800]=0;
    char page[3][ONLINE_LINE_BYTES];
    assert(online_caption_page(text,42,page)==43);
    assert(strlen(page[0])==14 && strlen(page[2])==8);
    assert(online_caption_page("一二三四五六七八九十甲乙丙丁戊",0,page)==1);
    assert(!strcmp(page[0],"一二三四五六七八九十甲") && !strcmp(page[1],"乙丙丁戊"));
    /* A 43-character answer has a short fourth row. Its second page must
     * avoid overlapping text and one delayed character on its own. */
    for(unsigned i=0;i<43;++i)text[i]=(char)('A'+i%26);
    text[43]=0;
    assert(online_caption_page(text,0,page)==2);
    assert(strlen(page[0])==14 && strlen(page[1])==14 && strlen(page[2])==11);
    char tail[3][ONLINE_LINE_BYTES];
    assert(online_caption_page(text,1,tail)==2);
    assert(strlen(tail[0])==4 && !tail[1][0] && !tail[2][0]);
    char joined[64];snprintf(joined,sizeof(joined),"%s%s%s%s",page[0],page[1],page[2],tail[0]);
    assert(!strcmp(joined,text));
    assert(online_caption_page("",0,page)==0 && !page[0][0]);
    assert(online_caption_slice("a\nb",0,page,3)==2);
    assert(!strcmp(page[0],"a") && !strcmp(page[1],"b"));
    assert(online_caption_slice("你好朋友\n呀",0,page,3)==2);
    assert(!strcmp(page[0],"你好朋友")); /* Never create a new orphan before a hard break. */
    for(unsigned row=0;row<3;++row)strcpy(page[row],"一二三四五六七八九十甲乙丙丁");
    online_caption_cursor c={0};
    uint32_t starts[]={0,9*24000,15*24000};
    online_caption_update(&c,1,0,false,0,starts,3);
    online_caption_update(&c,1,8*24000,true,8000,starts,3);
    assert(c.active==0); /* Slow speech stays on the page being spoken. */
    online_caption_update(&c,1,9*24000,true,9000,starts,3);assert(c.active==1);
    online_caption_update(&c,1,14*24000,true,14000,starts,3);assert(c.active==1);
    online_caption_update(&c,1,14*24000,true,24000,starts,3);assert(c.active==1); /* Network stall. */
    online_caption_update(&c,1,15*24000,true,25000,starts,3);assert(c.active==2);
    online_caption_move(&c,-1,25000);assert(c.active==1 && c.manual);
    online_caption_update(&c,1,20*24000,true,30999,starts,3);assert(c.active==1);
    online_caption_update(&c,1,20*24000,true,31000,starts,3);assert(c.active==2 && !c.manual);
    online_caption_move(&c,-1,32000);
    online_caption_update(&c,1,20*24000,false,50000,starts,3);assert(c.active==1 && c.manual);
    online_caption_update(&c,2,0,false,51000,starts,3);assert(!c.active && !c.manual);
    starts[1]=UINT32_MAX;
    online_caption_update(&c,2,30*24000,false,52000,starts,3);assert(!c.active); /* Never guess or jump at audio.done. */
    starts[1]=9*24000;
    online_caption_update(&c,3,0,true,UINT32_MAX-200,starts,3);
    online_caption_move(&c,1,UINT32_MAX-200);
    online_caption_update(&c,3,0,true,49,starts,3);assert(c.manual && c.active==1);
    online_caption_update(&c,3,10*24000,true,5799,starts,3);assert(!c.manual && c.active==1);
    assert(online_caption_boundary(text,1)==39); /* Four-row orphan balancing. */
    const char *quote="“今天慢慢来，也没有关系。”她说：“认真生活的你已经很棒啦！”";
    char quoted[8][ONLINE_LINE_BYTES],joined_quote[256]="";
    unsigned rows=online_caption_lines(quote,quoted,8);
    for(unsigned i=0;i<rows;++i){strcat(joined_quote,quoted[i]);assert(!strstr(quoted[i],"“") || strcmp(quoted[i]+strlen(quoted[i])-3,"“"));}
    assert(!strcmp(quote,joined_quote));

}
static void contiguous_pages(void) {
    char text[1801],joined[1801],page[3][ONLINE_LINE_BYTES];
    for(unsigned length=1;length<=1800;length+=13) {
        for(unsigned i=0;i<length;++i)text[i]=(char)('a'+i%26);
        text[length]=0;joined[0]=0;
        unsigned pages=online_caption_page(text,0,page);
        for(unsigned p=0;p<pages;++p) {
            assert(online_caption_boundary(text,p)==strlen(joined));
            assert(online_caption_page(text,p,page)==pages);
            if(p+1<pages)assert(page[0][0] && page[1][0] && page[2][0]);
            for(unsigned row=0;row<3;++row)strcat(joined,page[row]);
        }
        assert(!strcmp(text,joined));
    }
}
int main(void) {
    punctuation();
    captions();
    contiguous_pages();
    assert(online_caption_top(3)==173);
    assert(online_caption_top(2)==183);
    assert(online_caption_top(1)==193);
    assert(online_caption_top(99)==173);
    online_config_t c={.version=1,.ssid="test-network",.password="test-only",.url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen-audio-3.0-realtime-plus",.token="test-placeholder-not-a-key"};
    assert(online_config_valid(&c));
    c.password[3]=0;assert(!online_config_valid(&c));c.password[0]=0;assert(online_config_valid(&c));
    c.url[6]='x';assert(!online_config_valid(&c));
    size_t used=0;assert(online_fragment(&used,100,0,25,60));assert(online_fragment(&used,100,25,35,60));
    assert(!online_fragment(&used,100,0,100,100));assert(!online_fragment(&used,100,2,5,7));
    char lines[4][ONLINE_LINE_BYTES];assert(online_caption_lines("一二三四五六七八九十甲乙丙丁戊",lines,4)==2);
    assert(!strcmp(lines[0],"一二三四五六七八九十甲") && !strcmp(lines[1],"乙丙丁戊"));
    assert(online_caption_lines("你好💗今天也加油",lines,4)==1 && !strcmp(lines[0],"你好今天也加油"));
    assert(online_caption_lines("",lines,4)==0);assert(online_caption_lines("hello",lines,0)==0);
    const char partial[]={'a',(char)0xe4,(char)0xbd,0};assert(online_caption_lines(partial,lines,4)==1 && !strcmp(lines[0],"a"));
    char audio_json[]={"{\"type\":\"response.audio.delta\",\"delta\":\"AQIDBA==\",\"output_index\":0}"};
    char *audio;size_t audio_length;
    assert(online_audio_event(audio_json,strlen(audio_json),&audio,&audio_length)==1 && audio_length==8 && !memcmp(audio,"AQIDBA==",8));
    char swapped[]={"{\"delta\":\"AA\\/A\",\"meta\":{\"delta\":\"bad\"},\"type\":\"response.audio.delta\"}"};
    assert(online_audio_event(swapped,strlen(swapped),&audio,&audio_length)==1 && audio_length==4 && !memcmp(audio,"AA/A",4));
    char invalid[]={"{\"type\":\"response.audio.delta\",\"delta\":\"?!==\"}"};
    assert(online_audio_event(invalid,strlen(invalid),&audio,&audio_length)==-1);
    char truncated[]={"{\"type\":\"response.audio.delta\",\"delta\":\"AAAA"};
    assert(online_audio_event(truncated,strlen(truncated),&audio,&audio_length)==-1);
    char other[]={"{\"type\":\"response.audio.done\"}"};
    assert(online_audio_event(other,strlen(other),&audio,&audio_length)==0);
    static char large[30000];const char *prefix="{\"delta\":\"";size_t prefix_size=strlen(prefix);memcpy(large,prefix,prefix_size);memset(large+prefix_size,'A',25800);strcpy(large+prefix_size+25800,"\",\"type\":\"response.audio.delta\"}");
    assert(online_audio_event(large,strlen(large),&audio,&audio_length)==1 && audio_length==25800);
    puts("Online config, bounded fragments, UTF-8 and orphan captions PASS");
}
