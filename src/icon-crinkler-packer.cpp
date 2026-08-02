#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstring>
#include "icon-crinkler-packer.h"

namespace {
struct Parsed {
    const IMAGE_NT_HEADERS32* nt=nullptr;
    const IMAGE_SECTION_HEADER* sections=nullptr;
    const IMAGE_SECTION_HEADER* resource=nullptr;
};
struct ResourceLeaf {std::vector<uint8_t> bytes;uint16_t language=0;};

bool Range(size_t at,size_t size,size_t total){return at<=total&&size<=total-at;}
void Fail(std::wstring* error,const wchar_t* text){if(error)*error=text;}
void Put16(std::vector<uint8_t>& bytes,uint32_t at,uint16_t value){memcpy(bytes.data()+at,&value,2);}
void Put32(std::vector<uint8_t>& bytes,uint32_t at,uint32_t value){memcpy(bytes.data()+at,&value,4);}

const IMAGE_SECTION_HEADER* RvaSection(const Parsed& parsed,uint32_t rva){
    for(unsigned i=0;i<parsed.nt->FileHeader.NumberOfSections;++i){
        const auto& section=parsed.sections[i];
        uint32_t size=std::max(section.Misc.VirtualSize,section.SizeOfRawData);
        if(rva>=section.VirtualAddress&&rva-section.VirtualAddress<size)return &section;
    }
    return nullptr;
}

bool Parse(const std::vector<uint8_t>& bytes,Parsed& parsed,std::wstring* error){
    if(bytes.size()<sizeof(IMAGE_DOS_HEADER)){Fail(error,L"truncated DOS header");return false;}
    const auto* dos=(const IMAGE_DOS_HEADER*)bytes.data();
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||!Range((size_t)dos->e_lfanew,sizeof(IMAGE_NT_HEADERS32),bytes.size())){Fail(error,L"invalid PE signature offset");return false;}
    parsed.nt=(const IMAGE_NT_HEADERS32*)(bytes.data()+dos->e_lfanew);
    if(parsed.nt->Signature!=IMAGE_NT_SIGNATURE||parsed.nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||parsed.nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||parsed.nt->FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER32)){Fail(error,L"not a supported PE32 image");return false;}
    size_t table=(size_t)((const uint8_t*)&parsed.nt->OptionalHeader-bytes.data())+parsed.nt->FileHeader.SizeOfOptionalHeader;
    if(!Range(table,(size_t)parsed.nt->FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER),bytes.size())){Fail(error,L"truncated section table");return false;}
    parsed.sections=(const IMAGE_SECTION_HEADER*)(bytes.data()+table);
    uint32_t resourceRva=parsed.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
    parsed.resource=resourceRva?RvaSection(parsed,resourceRva):nullptr;
    if(!parsed.resource||!parsed.resource->SizeOfRawData||!Range(parsed.resource->PointerToRawData,parsed.resource->SizeOfRawData,bytes.size())){Fail(error,L"source has no valid icon resource section");return false;}
    return true;
}

bool ResourceDirectoryAt(const std::vector<uint8_t>& bytes,uint32_t at,const IMAGE_RESOURCE_DIRECTORY*& directory,const IMAGE_RESOURCE_DIRECTORY_ENTRY*& entries){
    if(!Range(at,sizeof(*directory),bytes.size()))return false;
    directory=(const IMAGE_RESOURCE_DIRECTORY*)(bytes.data()+at);
    uint32_t count=(uint32_t)directory->NumberOfNamedEntries+directory->NumberOfIdEntries;
    uint32_t entriesAt=at+sizeof(*directory);
    if(!Range(entriesAt,(size_t)count*sizeof(*entries),bytes.size()))return false;
    entries=(const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(bytes.data()+entriesAt);
    return true;
}

bool FindResourceChild(const std::vector<uint8_t>& bytes,uint32_t directoryAt,uint16_t wanted,bool any,IMAGE_RESOURCE_DIRECTORY_ENTRY& found){
    const IMAGE_RESOURCE_DIRECTORY* directory=nullptr;const IMAGE_RESOURCE_DIRECTORY_ENTRY* entries=nullptr;
    if(!ResourceDirectoryAt(bytes,directoryAt,directory,entries))return false;
    uint32_t count=(uint32_t)directory->NumberOfNamedEntries+directory->NumberOfIdEntries;
    if(!count)return false;
    if(any){found=entries[0];return true;}
    for(uint32_t i=directory->NumberOfNamedEntries;i<count;++i)if(!entries[i].NameIsString&&entries[i].Id==wanted){found=entries[i];return true;}
    return false;
}

bool ReadResourceLeaf(const std::vector<uint8_t>& resource,uint32_t sectionRva,uint16_t type,uint16_t name,bool anyName,ResourceLeaf& leaf){
    IMAGE_RESOURCE_DIRECTORY_ENTRY entry{};
    if(!FindResourceChild(resource,0,type,false,entry)||!entry.DataIsDirectory)return false;
    uint32_t typeAt=entry.OffsetToDirectory;
    if(!FindResourceChild(resource,typeAt,name,anyName,entry)||!entry.DataIsDirectory)return false;
    uint32_t nameAt=entry.OffsetToDirectory;
    if(!FindResourceChild(resource,nameAt,0,true,entry)||entry.DataIsDirectory)return false;
    const IMAGE_RESOURCE_DIRECTORY* languageDirectory=nullptr;const IMAGE_RESOURCE_DIRECTORY_ENTRY* languages=nullptr;
    if(!ResourceDirectoryAt(resource,nameAt,languageDirectory,languages))return false;
    leaf.language=languages[0].NameIsString?0:languages[0].Id;
    uint32_t dataAt=entry.OffsetToData;
    if(!Range(dataAt,sizeof(IMAGE_RESOURCE_DATA_ENTRY),resource.size()))return false;
    const auto* data=(const IMAGE_RESOURCE_DATA_ENTRY*)(resource.data()+dataAt);
    if(data->OffsetToData<sectionRva||!Range(data->OffsetToData-sectionRva,data->Size,resource.size()))return false;
    leaf.bytes.assign(resource.begin()+data->OffsetToData-sectionRva,resource.begin()+data->OffsetToData-sectionRva+data->Size);
    return true;
}
}

bool PackCrinklerIconPe(const std::vector<uint8_t>& source,const std::vector<uint8_t>& core,std::vector<uint8_t>& packed,std::wstring* error){
    // Bytes 132..147 simultaneously form the resource root header and the PE
    // import/resource directory fields. In particular, the nominal resource
    // size 0x20000 encodes zero named and two integer root entries. Windows'
    // resource walkers use the tree's offsets and leaf sizes, not that bound.
    constexpr uint32_t Split=148,ResourceRoot=132,RootPrefixBytes=16;
    constexpr uint32_t IconEntryAt=80,IconAt=88,ImageBase=0x400000;
    constexpr uint32_t PackedPointer=15,NoMatchJump=101,AritJump=109,MatchBack=193,DecodeBack=255;

    packed.clear();Parsed parsed;
    if(!Parse(source,parsed,error))return false;
    auto u16=[&](uint32_t at){uint16_t value=0;if(Range(at,2,core.size()))memcpy(&value,core.data()+at,2);return value;};
    auto u32=[&](uint32_t at){uint32_t value=0;if(Range(at,4,core.size()))memcpy(&value,core.data()+at,4);return value;};
    if(core.size()<281||u16(0)!=IMAGE_DOS_SIGNATURE||u32(0x3c)!=4||u32(4)!=IMAGE_NT_SIGNATURE||u16(8)!=IMAGE_FILE_MACHINE_I386||u16(10)||u16(24)!=120||u32(44)!=Split||u32(88)!=48||u32(120)!=3||u32(140)||u32(144)||core[12]!=0x0f||core[13]!=0xa3||core[14]!=0x2d||core[100]!=0xe9||core[108]!=0xe9||core[192]!=0xe9||core[254]!=0xe9){Fail(error,L"invalid resource-capable Crinkler core");return false;}
    uint32_t oldPacked=u32(PackedPointer);
    if(oldPacked<ImageBase+256||oldPacked-ImageBase>=core.size()){Fail(error,L"invalid Crinkler packed-data pointer");return false;}

    uint32_t resourceBytes=parsed.resource->Misc.VirtualSize;
    if(resourceBytes>parsed.resource->SizeOfRawData){Fail(error,L"source resource section is truncated");return false;}
    std::vector<uint8_t> original(source.begin()+parsed.resource->PointerToRawData,source.begin()+parsed.resource->PointerToRawData+resourceBytes);
    ResourceLeaf group,icon;
    if(!ReadResourceLeaf(original,parsed.resource->VirtualAddress,14,0,true,group)||group.bytes.size()!=20){Fail(error,L"source does not contain one compact group icon");return false;}
    uint16_t iconId=0;memcpy(&iconId,group.bytes.data()+18,2);
    if(!iconId||!ReadResourceLeaf(original,parsed.resource->VirtualAddress,3,iconId,false,icon)||icon.bytes.empty()){Fail(error,L"source group icon has no image");return false;}

    uint32_t groupEntryAt=IconAt+(uint32_t)icon.bytes.size(),groupAt=groupEntryAt+8;
    uint32_t logicalBytes=groupAt+(uint32_t)group.bytes.size(),carrierBytes=logicalBytes-RootPrefixBytes;
    if(logicalBytes>16*1024*1024||core.size()>UINT32_MAX-carrierBytes||Split+carrierBytes>u32(84)){Fail(error,L"icon carrier does not fit in the Crinkler image");return false;}
    std::vector<uint8_t> logical(logicalBytes,0);
    Put16(logical,14,2);Put32(logical,16,3);Put32(logical,20,0x80000000u|20);Put32(logical,24,14);Put32(logical,28,0x80000000u|32);
    // Every child directory's ignored 12-byte prefix overlaps preceding
    // counts/entries. Each data entry's ignored code-page/reserved half is the
    // first eight bytes of its resource data.
    Put16(logical,34,1);Put32(logical,36,1);Put32(logical,40,0x80000000u|44);
    Put16(logical,46,1);Put32(logical,48,1);Put32(logical,52,0x80000000u|56);
    Put16(logical,58,1);Put32(logical,60,icon.language);Put32(logical,64,IconEntryAt);
    Put16(logical,70,1);Put32(logical,72,group.language);Put32(logical,76,groupEntryAt);
    Put32(logical,IconEntryAt,ResourceRoot+IconAt);Put32(logical,IconEntryAt+4,(uint32_t)icon.bytes.size());
    Put32(logical,groupEntryAt,ResourceRoot+groupAt);Put32(logical,groupEntryAt+4,(uint32_t)group.bytes.size());
    memcpy(logical.data()+IconAt,icon.bytes.data(),icon.bytes.size());
    memcpy(logical.data()+groupAt,group.bytes.data(),group.bytes.size());
    Put32(logical,groupAt+14,(uint32_t)icon.bytes.size());Put16(logical,groupAt+18,1);

    packed.reserve(core.size()+carrierBytes);
    packed.insert(packed.end(),core.begin(),core.begin()+Split);
    packed.insert(packed.end(),logical.begin()+RootPrefixBytes,logical.end());
    packed.insert(packed.end(),core.begin()+Split,core.end());
    Put32(packed,44,Split+carrierBytes);Put32(packed,88,Split+carrierBytes);
    Put32(packed,140,ResourceRoot);Put32(packed,144,0x20000);Put32(packed,PackedPointer,oldPacked+carrierBytes);
    auto adjustSigned=[&](uint32_t at,int32_t delta){int32_t value=0;memcpy(&value,packed.data()+at,4);value+=delta;memcpy(packed.data()+at,&value,4);};
    adjustSigned(NoMatchJump,(int32_t)carrierBytes);adjustSigned(AritJump,(int32_t)carrierBytes);
    adjustSigned(MatchBack+carrierBytes,-(int32_t)carrierBytes);adjustSigned(DecodeBack+carrierBytes,-(int32_t)carrierBytes);
    return true;
}
