#include "icon-png-optimizer.h"
#include "../third_party/zlib/zlib.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <set>

namespace {
using Bytes=std::vector<uint8_t>;
struct Image {uint32_t width,height;unsigned depth,type;Bytes samples,palette,transparency;};

uint32_t ReadColor(const uint8_t* p){
    if(!p[3])return 0; // Hidden RGB is not visible and impairs palette reduction.
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
void Put32(Bytes& out,uint32_t value){out.push_back((uint8_t)(value>>24));out.push_back((uint8_t)(value>>16));out.push_back((uint8_t)(value>>8));out.push_back((uint8_t)value);}
void Chunk(Bytes& out,const char type[5],const Bytes& data){
    Put32(out,(uint32_t)data.size());size_t start=out.size();out.insert(out.end(),type,type+4);out.insert(out.end(),data.begin(),data.end());
    Put32(out,(uint32_t)crc32(0,out.data()+start,(uInt)(out.size()-start)));
}
unsigned Paeth(unsigned a,unsigned b,unsigned c){int p=(int)a+(int)b-(int)c,pa=abs(p-(int)a),pb=abs(p-(int)b),pc=abs(p-(int)c);return pa<=pb&&pa<=pc?a:pb<=pc?b:c;}
Bytes Filter(const Image& image,int fixed){
    size_t rowBytes=((size_t)image.width*image.depth+7)/8;Bytes out((rowBytes+1)*image.height);
    for(uint32_t y=0;y<image.height;++y){const uint8_t* row=image.samples.data()+rowBytes*y;const uint8_t* prior=y?row-rowBytes:nullptr;int selected=fixed;
        if(selected<0){unsigned best=UINT_MAX;for(int f=0;f<5;++f){unsigned score=0;for(size_t x=0;x<rowBytes;++x){unsigned left=x?row[x-1]:0,up=prior?prior[x]:0,ul=prior&&x?prior[x-1]:0,predict=f==0?0:f==1?left:f==2?up:f==3?(left+up)/2:Paeth(left,up,ul);int d=(uint8_t)(row[x]-predict);score+=(unsigned)abs(d<128?d:d-256);}if(score<best){best=score;selected=f;}}}
        uint8_t* dst=out.data()+y*(rowBytes+1);dst[0]=(uint8_t)selected;for(size_t x=0;x<rowBytes;++x){unsigned left=x?row[x-1]:0,up=prior?prior[x]:0,ul=prior&&x?prior[x-1]:0,predict=selected==0?0:selected==1?left:selected==2?up:selected==3?(left+up)/2:Paeth(left,up,ul);dst[x+1]=(uint8_t)(row[x]-predict);}
    }return out;
}
bool Deflate(const Bytes& input,int level,int strategy,int windowBits,Bytes& output){
    z_stream stream{};if(deflateInit2(&stream,level,Z_DEFLATED,windowBits,8,strategy)!=Z_OK)return false;
    uLong bound=deflateBound(&stream,(uLong)input.size());if(bound>UINT_MAX){deflateEnd(&stream);return false;}output.resize((size_t)bound);stream.next_in=const_cast<Bytef*>(input.data());stream.avail_in=(uInt)input.size();stream.next_out=output.data();stream.avail_out=(uInt)output.size();int result=deflate(&stream,Z_FINISH);bool ok=result==Z_STREAM_END;if(ok)output.resize(stream.total_out);deflateEnd(&stream);return ok;
}
Bytes Serialize(const Image& image,const Bytes& compressed){
    static const uint8_t signature[]={137,80,78,71,13,10,26,10};Bytes png(signature,signature+8),ihdr;Put32(ihdr,image.width);Put32(ihdr,image.height);ihdr.push_back((uint8_t)image.depth);ihdr.push_back((uint8_t)image.type);ihdr.insert(ihdr.end(),{0,0,0});Chunk(png,"IHDR",ihdr);if(!image.palette.empty())Chunk(png,"PLTE",image.palette);if(!image.transparency.empty())Chunk(png,"tRNS",image.transparency);Chunk(png,"IDAT",compressed);Chunk(png,"IEND",{});return png;
}
void Keep(Bytes candidate,Bytes& best){if(best.empty()||candidate.size()<best.size()||(candidate.size()==best.size()&&candidate<best))best.swap(candidate);}
void CompressImage(const Image& image,Bytes& best,bool quick=false){
    const int filters[]={0,1,2,3,4,-1};const int levels[]={1,6,9};const int strategies[]={Z_DEFAULT_STRATEGY,Z_FILTERED,Z_RLE};const int windows[]={9,10,11,15};
    int filterCount=quick?1:6;for(int fi=0;fi<filterCount;++fi){Bytes filtered=Filter(image,filters[fi]);for(int wi=0;wi<(quick?1:4);++wi)for(int si=0;si<(quick?1:3);++si)for(int li=quick?1:0;li<(quick?2:3);++li){Bytes compressed;if(Deflate(filtered,levels[li],strategies[si],windows[wi],compressed))Keep(Serialize(image,compressed),best);}}
}
unsigned DepthFor(size_t colors){return colors<=2?1:colors<=4?2:colors<=16?4:8;}
Image Indexed(uint32_t width,uint32_t height,const std::vector<uint32_t>& pixels,const std::vector<uint32_t>& order){
    Image image{width,height,DepthFor(order.size()),3};for(uint32_t value:order){image.palette.push_back((uint8_t)(value>>16));image.palette.push_back((uint8_t)(value>>8));image.palette.push_back((uint8_t)value);}size_t trns=0;for(size_t i=0;i<order.size();++i)if((uint8_t)(order[i]>>24)!=255)trns=i+1;for(size_t i=0;i<trns;++i)image.transparency.push_back((uint8_t)(order[i]>>24));
    size_t rowBytes=((size_t)width*image.depth+7)/8;image.samples.assign(rowBytes*height,0);unsigned mask=(1u<<image.depth)-1;for(size_t p=0;p<pixels.size();++p){unsigned index=(unsigned)(std::find(order.begin(),order.end(),pixels[p])-order.begin());size_t y=p/width,x=p%width,bit=x*image.depth;image.samples[y*rowBytes+bit/8]|=(uint8_t)((index&mask)<<(8-image.depth-bit%8));}return image;
}
bool Grayscale(uint32_t width,uint32_t height,const uint8_t* bgra,Image& image){
    unsigned depth=1;int transparent=-1;for(size_t p=0;p<(size_t)width*height;++p){const uint8_t* c=bgra+p*4;unsigned gray=c[3]?c[0]:0;if((c[3]&&(c[0]!=c[1]||c[1]!=c[2]))||(c[3]!=0&&c[3]!=255))return false;if(!c[3]){if(transparent>=0&&transparent!=(int)gray)return false;transparent=(int)gray;}}
    for(unsigned candidate:{1u,2u,4u,8u}){unsigned max=(1u<<candidate)-1;bool exact=true;for(size_t p=0;p<(size_t)width*height;++p)if((unsigned)(bgra[p*4+3]?bgra[p*4]:0)*max%255){exact=false;break;}if(exact){depth=candidate;break;}}
    if(transparent>=0)for(size_t p=0;p<(size_t)width*height;++p)if(bgra[p*4+3]&&bgra[p*4]==transparent)return false;
    image={width,height,depth,0};size_t rowBytes=((size_t)width*depth+7)/8;image.samples.assign(rowBytes*height,0);unsigned max=(1u<<depth)-1;for(size_t p=0;p<(size_t)width*height;++p){unsigned sample=(unsigned)(bgra[p*4+3]?bgra[p*4]:0)*max/255,x=(unsigned)(p%width),bit=x*depth;image.samples[(p/width)*rowBytes+bit/8]|=(uint8_t)(sample<<(8-depth-bit%8));}if(transparent>=0){unsigned sample=(unsigned)transparent*max/255;image.transparency={(uint8_t)(sample>>8),(uint8_t)sample};}return true;
}
}

bool TinyPngPixelsEqual(const Bytes& a,const Bytes& b){if(a.size()!=b.size()||a.size()%4)return false;for(size_t i=0;i<a.size();i+=4){if(a[i+3]!=b[i+3])return false;if(a[i+3]&&memcmp(a.data()+i,b.data()+i,3))return false;}return true;}

bool OptimizeTinyPng(uint32_t width,uint32_t height,const uint8_t* bgra,size_t size,Bytes& png){
    png.clear();if(!width||!height||width>256||height>256||!bgra||(size_t)width*height>SIZE_MAX/4||size!=(size_t)width*height*4)return false;
    Image gray;if(Grayscale(width,height,bgra,gray))CompressImage(gray,png);
    std::vector<uint32_t> pixels(size/4),colors;std::vector<unsigned> frequency;for(size_t i=0;i<pixels.size();++i){uint32_t color=ReadColor(bgra+i*4);pixels[i]=color;auto found=std::find(colors.begin(),colors.end(),color);if(found==colors.end()){if(colors.size()==256)return !png.empty();colors.push_back(color);frequency.push_back(1);}else ++frequency[(size_t)(found-colors.begin())];}
    std::set<std::vector<uint32_t>> orders;orders.insert(colors);auto addSorted=[&](auto less){auto order=colors;std::stable_sort(order.begin(),order.end(),less);orders.insert(order);};
    addSorted([](uint32_t a,uint32_t b){return (uint8_t)(a>>24)<(uint8_t)(b>>24);});addSorted([](uint32_t a,uint32_t b){return a<b;});addSorted([&](uint32_t a,uint32_t b){auto ia=(size_t)(std::find(colors.begin(),colors.end(),a)-colors.begin()),ib=(size_t)(std::find(colors.begin(),colors.end(),b)-colors.begin());return frequency[ia]>frequency[ib];});
    if(colors.size()<=8){auto order=colors;std::sort(order.begin(),order.end());do{orders.insert(order);}while(std::next_permutation(order.begin(),order.end()));}
    struct Ranked {size_t bytes,trns;std::vector<uint32_t> order;};std::vector<Ranked> ranked;for(const auto& order:orders){Bytes score;Image image=Indexed(width,height,pixels,order);CompressImage(image,score,true);ranked.push_back({score.size(),image.transparency.size(),order});}std::sort(ranked.begin(),ranked.end(),[](const Ranked& a,const Ranked& b){return std::tie(a.bytes,a.trns,a.order)<std::tie(b.bytes,b.trns,b.order);});
    size_t limit=std::min<size_t>(12,ranked.size());for(size_t i=0;i<limit;++i)CompressImage(Indexed(width,height,pixels,ranked[i].order),png);if(!ranked.empty()){auto minTrns=*std::min_element(ranked.begin(),ranked.end(),[](const Ranked& a,const Ranked& b){return std::tie(a.trns,a.bytes)<std::tie(b.trns,b.bytes);});CompressImage(Indexed(width,height,pixels,minTrns.order),png);}return !png.empty();
}
