#pragma once
// Apex-original MIT native CIL rewriter for CASHair.PopulateTypesGrid(bool).
//
// Creates a DETACHED, validated candidate IL body plus its two finally
// clauses. It never writes to a Mono runtime, a UI.dll, or an executable.
// The original game method, loaded-image identity, interpreter translation
// state, and ownership must be verified separately before using this buffer.
// In particular Mono's MINT RuntimeMethod is NOT a native JIT function.
// Unknown builds and other methods fail closed. No game bytes are embedded.
#include <algorithm>
#include <array>
#include <cstddef>
#include <climits>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace ApexCasNativeIl {
enum class Variant : std::uint8_t { Unknown, Ea169, AlternateUI };
struct Rewritten {
    std::vector<std::uint8_t> il;
    std::vector<std::uint8_t> eh;
    std::array<std::uint8_t, 32> sourceDigest{};
    bool validated = false;
};
namespace Detail {
inline std::uint16_t U16(std::span<const std::uint8_t> s, std::size_t at) {
    return std::uint16_t(s[at] | (std::uint16_t(s[at+1]) << 8));
}
inline std::uint32_t U32(std::span<const std::uint8_t> s, std::size_t at) {
    return std::uint32_t(s[at]) | (std::uint32_t(s[at+1])<<8) |
           (std::uint32_t(s[at+2])<<16) | (std::uint32_t(s[at+3])<<24);
}
inline void Push32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (unsigned k=0;k<4;++k) o.push_back(std::uint8_t(v >> (k*8)));
}
inline std::uint32_t Ror(std::uint32_t a, unsigned n) {
    return (a >> n) | (a << (32-n));
}
// Local SHA-256 for tiny, immutable IL snapshots. No CryptoAPI dependency,
// heap-scanned pointers or signature-derived approvals.
inline std::array<std::uint8_t,32> SHA256(std::span<const std::uint8_t> data) {
    constexpr std::array<std::uint32_t,64> k{{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    }};
    std::array<std::uint32_t,8> h{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    const std::uint64_t bitLen=std::uint64_t(data.size())*8;
    const std::size_t padded=(data.size()+9+63)&~std::size_t(63);
    std::vector<std::uint8_t> bytes(padded,0);
    std::copy(data.begin(),data.end(),bytes.begin());
    bytes[data.size()]=0x80;
    for(unsigned j=0;j<8;++j) bytes[padded-8+j]=std::uint8_t(bitLen>>((7-j)*8));
    for(std::size_t at=0;at<padded;at+=64) {
        std::uint32_t w[64]{};
        for(unsigned i=0;i<16;++i)
            w[i]=(std::uint32_t(bytes[at+i*4])<<24)|
                 (std::uint32_t(bytes[at+i*4+1])<<16)|
                 (std::uint32_t(bytes[at+i*4+2])<<8)|
                  std::uint32_t(bytes[at+i*4+3]);
        for(unsigned i=16;i<64;++i) {
            const std::uint32_t s0=Ror(w[i-15],7)^Ror(w[i-15],18)^(w[i-15]>>3);
            const std::uint32_t s1=Ror(w[i-2],17)^Ror(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(unsigned i=0;i<64;++i) {
            const std::uint32_t s1=Ror(e,6)^Ror(e,11)^Ror(e,25);
            const std::uint32_t choice=(e&f)^((~e)&g);
            const std::uint32_t t1=hh+s1+choice+k[i]+w[i];
            const std::uint32_t s0=Ror(a,2)^Ror(a,13)^Ror(a,22);
            const std::uint32_t majority=(a&b)^(a&c)^(b&c);
            const std::uint32_t t2=s0+majority;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        const std::uint32_t x[8]{a,b,c,d,e,f,g,hh};
        for(unsigned i=0;i<8;++i) h[i]+=x[i];
    }
    std::array<std::uint8_t,32> result{};
    for(unsigned i=0;i<8;++i)
        for(unsigned j=0;j<4;++j)
            result[i*4+j]=std::uint8_t(h[i]>>((3-j)*8));
    return result;
}
inline bool DigestEquals(std::span<const std::uint8_t> input,
                         const char* expected) {
    const auto bytes=SHA256(input);
    constexpr char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i) {
        if(expected[i*2]!=hex[bytes[i]>>4] ||
           expected[i*2+1]!=hex[bytes[i]&15]) return false;
    }
    return expected[64]=='\0';
}
struct Op {
    std::size_t start=0, len=0;
    std::uint16_t code=0;
    std::vector<std::size_t> targets;
};
inline bool Short(std::uint16_t op) {return (op>=0x2b&&op<=0x37)||op==0xde;}
inline bool Long(std::uint16_t op) {return (op>=0x38&&op<=0x44)||op==0xdd;}
inline bool Token(std::uint16_t x) {
    if(x>=0x27&&x<=0x29) return true;
    switch(x) {
        case 0x6f:case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:
        case 0x75:case 0x79:case 0x7b:case 0x7c:case 0x7d:case 0x7e:
        case 0x7f:case 0x80:case 0x81:case 0x8c:case 0x8d:case 0x8f:
        case 0xa3:case 0xa4:case 0xa5:case 0xc2:case 0xc6:case 0xd0:
        case 0xfe06:case 0xfe07:case 0xfe15:case 0xfe16:case 0xfe1c:return true;
    }
    return false;
}
inline bool NoOperand(std::uint16_t op) {
    if(op<=0x0d||(op>=0x14&&op<=0x1e)||(op>=0x46&&op<=0x6e)||
       (op>=0x82&&op<=0x8b)||(op>=0x90&&op<=0xa2)||
       (op>=0xb3&&op<=0xba)||(op>=0xd1&&op<=0xdb)) return true;
    switch(op) {
        case 0x25:case 0x26:case 0x2a:case 0x76:case 0x7a:case 0x8e:
        case 0xdc:case 0xdf:case 0xe0:
        case 0xfe00:case 0xfe01:case 0xfe02:case 0xfe03:case 0xfe04:
        case 0xfe05:case 0xfe0f:case 0xfe11:case 0xfe13:case 0xfe14:
        case 0xfe17:case 0xfe18:case 0xfe1a:case 0xfe1d:case 0xfe1e:return true;
    }
    return false;
}
inline bool Decode(std::span<const std::uint8_t> il,std::vector<Op>& ops) {
    ops.clear();
    std::size_t i=0;
    while(i<il.size()) {
        Op op;op.start=i;
        std::uint16_t code=il[i++];
        if(code==0xfe) {
            if(i==il.size()) return false;
            code=std::uint16_t(0xfe00|il[i++]);
        }
        op.code=code;
        std::size_t size=0;
        if((code>=0x0e&&code<=0x13)||code==0x1f||code==0xfe12||Short(code))
            size=1;
        else if(code>=0xfe09&&code<=0xfe0e) size=2;
        else if(code==0x20||code==0x22||Token(code)||Long(code)) size=4;
        else if(code==0x21||code==0x23) size=8;
        else if(code==0x45) {
            if(i+4>il.size())return false;
            const auto count=U32(il,i);
            if(count>(il.size()-i-4)/4) return false;
            size=4+std::size_t(count)*4;
        } else if(!NoOperand(code)) return false;
        if(size>il.size()-i)return false;
        const std::size_t end=i+size;
        if(Short(code)) {
            const std::int64_t target=std::int64_t(end)+
                std::int64_t(std::int8_t(il[i]));
            if(target<0 || target>std::int64_t(il.size()))return false;
            op.targets.push_back(std::size_t(target));
        } else if(Long(code)) {
            const std::int64_t target=std::int64_t(end)+
                std::int64_t(std::int32_t(U32(il,i)));
            if(target<0 || target>std::int64_t(il.size()))return false;
            op.targets.push_back(std::size_t(target));
        } else if(code==0x45) {
            const auto count=U32(il,i);
            for(std::uint32_t j=0;j<count;++j) {
                const std::int64_t target=std::int64_t(end)+
                    std::int64_t(std::int32_t(U32(il,i+4+j*4)));
                if(target<0 || target>std::int64_t(il.size()))return false;
                op.targets.push_back(std::size_t(target));
            }
        }
        op.len=end-op.start;
        ops.push_back(std::move(op));i=end;
    }
    std::vector<std::size_t> starts;
    for(const auto& op:ops)starts.push_back(op.start);
    starts.push_back(il.size());
    for(const auto& op:ops)
        for(const auto t:op.targets)
            if(!std::binary_search(starts.begin(),starts.end(),t))return false;
    return true;
}
struct Clause {std::uint32_t flags,start,len,handler,handlerLen,extra;};
inline bool Clauses(std::span<const std::uint8_t> raw,
                    std::size_t ilSize,std::array<Clause,2>& out) {
    if(raw.size()!=52||raw[0]!=0x41||raw[1]!=52||raw[2]||raw[3])return false;
    for(unsigned i=0;i<2;++i) {
        const auto at=4+i*24;
        auto& c=out[i];
        c={U32(raw,at),U32(raw,at+4),U32(raw,at+8),
           U32(raw,at+12),U32(raw,at+16),U32(raw,at+20)};
        if(c.flags!=2 || c.extra!=0 || c.start>ilSize ||
           c.len>ilSize-c.start || c.handler>ilSize ||
           c.handlerLen>ilSize-c.handler ||
           c.start+c.len>c.handler) return false;
    }
    return true;
}
inline std::uint16_t Wide(std::uint16_t op) {
    return op==0xde?0xdd:std::uint16_t(op+0x0d);
}
inline void Error(std::string* error,const char* message) {
    if(error)*error=message;
}
} // namespace Detail

// Used by offline tests and exclusively after the production SHA/shape gates.
// Exposed for synthetic fuzz/negative cases; do not supply an unknown live
// MonoMethod to this generic relocation routine.
inline bool Relocate(std::span<const std::uint8_t> original,
                     std::span<const std::uint8_t> exceptionSection,
                     std::size_t beforePc,std::span<const std::uint8_t> insertion,
                     Rewritten& out,std::string* error=nullptr) {
    out={};
    std::vector<Detail::Op> ops,inserted;
    std::array<Detail::Clause,2> clauses{};
    if(original.empty()||insertion.empty()||!Detail::Decode(original,ops)||
       !Detail::Decode(insertion,inserted)||
       !Detail::Clauses(exceptionSection,original.size(),clauses)) {
        Detail::Error(error,"invalid original IL, insertion, or finally clauses");
        return false;
    }
    const auto found=std::find_if(ops.begin(),ops.end(),
                                [beforePc](const auto& x){return x.start==beforePc;});
    if(found==ops.end() || inserted.size()!=2 ||
       inserted[0].code!=0x16 || inserted[1].code!=0x28 ||
       !inserted[0].targets.empty() || !inserted[1].targets.empty() ||
       insertion.size()!=6) {
        Detail::Error(error,"insertion must be exactly ldc.i4.0; call at opcode boundary");
        return false;
    }
    std::vector<std::size_t> newPc(original.size()+1,0);
    std::size_t len=0;
    for(const auto& op:ops) {
        if(op.start==beforePc)len+=insertion.size();
        newPc[op.start]=len;
        len+=Detail::Short(op.code)?5:op.len;
    }
    newPc[original.size()]=len;
    if(len>65536) {Detail::Error(error,"relocated method exceeds size bound");return false;}
    std::vector<std::uint8_t> buf;buf.reserve(len);
    for(const auto& op:ops) {
        if(op.start==beforePc)
            buf.insert(buf.end(),insertion.begin(),insertion.end());
        if(newPc[op.start]!=buf.size()) {
            Detail::Error(error,"IL position calculation mismatch");return false;
        }
        if(Detail::Short(op.code)||Detail::Long(op.code)) {
            const std::uint16_t wide=Detail::Short(op.code)?Detail::Wide(op.code):op.code;
            buf.push_back(std::uint8_t(wide));
            const std::int64_t delta=std::int64_t(newPc[op.targets[0]])-
                                     std::int64_t(buf.size()+4);
            if(delta<INT32_MIN||delta>INT32_MAX) {
                Detail::Error(error,"branch overflow");return false;
            }
            Detail::Push32(buf,std::uint32_t(std::int32_t(delta)));
        } else if(op.code==0x45) {
            buf.push_back(0x45);Detail::Push32(buf,std::uint32_t(op.targets.size()));
            const auto base=buf.size()+op.targets.size()*4;
            for(const auto target:op.targets) {
                const std::int64_t delta=std::int64_t(newPc[target])-std::int64_t(base);
                if(delta<INT32_MIN||delta>INT32_MAX)return false;
                Detail::Push32(buf,std::uint32_t(std::int32_t(delta)));
            }
        } else {
            buf.insert(buf.end(),original.begin()+op.start,
                       original.begin()+op.start+op.len);
        }
    }
    if(buf.size()!=len){Detail::Error(error,"relocated size mismatch");return false;}
    std::vector<std::uint8_t> eh{0x41,52,0,0};
    for(const auto& c:clauses) {
        auto start=c.start, tryEnd=std::size_t(c.start)+c.len;
        auto handler=c.handler, handlerEnd=std::size_t(c.handler)+c.handlerLen;
        const auto boundary=[&](std::size_t pos) {
            return pos==original.size()||
                   std::any_of(ops.begin(),ops.end(),
                               [pos](const Detail::Op& op){
                                   return op.start==pos;
                               });
        };
        if(!boundary(start)||!boundary(tryEnd)||!boundary(handler)||!boundary(handlerEnd)){
            Detail::Error(error,"finally boundary not at instruction");return false;
        }
        Detail::Push32(eh,c.flags);Detail::Push32(eh,std::uint32_t(newPc[start]));
        Detail::Push32(eh,std::uint32_t(newPc[tryEnd]-newPc[start]));
        Detail::Push32(eh,std::uint32_t(newPc[handler]));
        Detail::Push32(eh,std::uint32_t(newPc[handlerEnd]-newPc[handler]));
        Detail::Push32(eh,c.extra);
    }
    std::vector<Detail::Op> patched;
    if(!Detail::Decode(buf,patched)||patched.size()!=ops.size()+2) {
        Detail::Error(error,"relocated IL failed to decode");return false;
    }
    std::array<Detail::Clause,2> clausesAgain{};
    if(!Detail::Clauses(eh,buf.size(),clausesAgain)){
        Detail::Error(error,"relocated finally clauses invalid");return false;
    }
    // A well-formed rewritten byte stream is not enough: prove every
    // original branch still targets its original *instruction identity*,
    // including the original branches which skip the injected call.
    for(const auto& originalOp:ops) {
        const auto expectedPc=newPc[originalOp.start];
        const auto after=std::find_if(patched.begin(),patched.end(),
            [expectedPc](const Detail::Op& op){return op.start==expectedPc;});
        if(after==patched.end()) {
            Detail::Error(error,"an original instruction disappeared");return false;
        }
        const auto wanted=Detail::Short(originalOp.code)
                          ?Detail::Wide(originalOp.code):originalOp.code;
        if(after->code!=wanted ||
           after->targets.size()!=originalOp.targets.size()) {
            Detail::Error(error,"an original opcode changed");return false;
        }
        for(std::size_t j=0;j<originalOp.targets.size();++j) {
            if(after->targets[j]!=newPc[originalOp.targets[j]]) {
                Detail::Error(error,"an original branch was redirected");return false;
            }
        }
    }
    const auto source=Detail::SHA256(original);
    out.il=std::move(buf);out.eh=std::move(eh);
    out.sourceDigest=source;out.validated=true;
    return true;
}

inline bool BuildHairParent(std::span<const std::uint8_t> original,
                            std::span<const std::uint8_t> eh,
                            Variant variant,Rewritten& out,
                            std::string* error=nullptr) {
    out={};
    const char* digest=nullptr;
    std::uint32_t child=0,sleep=0;
    if(variant==Variant::Ea169) {
        digest="e0866c4327c0eec9ae55485aee7601e6168aa651b0f201e168c440dadc10bd15";
        child=0x0600191b;sleep=0x0a000023;
    } else if(variant==Variant::AlternateUI) {
        digest="020e281cc6f834ad0d58dfa26dfb8244245dd749224be19bb8504485a04c54c4";
        child=0x0600124d;sleep=0x0a00001d;
    }
    if(!digest||original.size()!=1621||!Detail::DigestEquals(original,digest)) {
        Detail::Error(error,"unknown or modified CASHair.PopulateTypesGrid original IL");
        return false;
    }
    std::vector<Detail::Op> ops;
    std::array<Detail::Clause,2> clauses{};
    if(!Detail::Decode(original,ops)||ops.size()!=528||
       !Detail::Clauses(eh,original.size(),clauses)) {
        Detail::Error(error,"original Hair/Hats IL or finally structure differs");
        return false;
    }
    const auto at=[&](std::size_t pc)->const Detail::Op*{
        auto it=std::find_if(ops.begin(),ops.end(),
                            [pc](const auto& x){return x.start==pc;});
        return it==ops.end()?nullptr:&*it;
    };
    const auto *loop=at(0x514), *endPreset=at(0x50f), *moveNext=at(0x516);
    if(!loop||loop->code!=0x11||!endPreset||endPreset->code!=0x3f||
       endPreset->targets!=std::vector<std::size_t>{0x42e}||
       !moveNext||moveNext->code!=0x6f) {
        Detail::Error(error,"original complete-part loop boundary differs");return false;
    }
    unsigned calls=0;std::vector<std::size_t> incoming;
    for(const auto& op:ops) {
        if(op.code==0x28 && op.len==5 &&
           Detail::U32(original,op.start+1)==child) {
            if(op.start!=0x3cc && op.start!=0x481) {
                Detail::Error(error,"unexpected AddHairTypeGridItem call site");return false;
            }
            ++calls;
        }
        for(const auto target:op.targets)
            if(target==0x514)incoming.push_back(op.start);
    }
    std::sort(incoming.begin(),incoming.end());
    if(calls!=2||incoming!=std::vector<std::size_t>{0x2ac,0x2f5,0x421}){
        Detail::Error(error,"Hair/Hats call sites or loop entries differ");return false;
    }
    const bool withinFinally=std::any_of(clauses.begin(),clauses.end(),
        [](const auto& c){return c.start<0x514&&0x514<c.start+c.len;});
    if(!withinFinally){Detail::Error(error,"loop boundary is not protected");return false;}
    std::array<std::uint8_t,6> insertion{0x16,0x28,
        std::uint8_t(sleep),std::uint8_t(sleep>>8),
        std::uint8_t(sleep>>16),std::uint8_t(sleep>>24)};
    if(!Relocate(original,eh,0x514,insertion,out,error))return false;
    if(out.il.size()!=1705||out.eh.size()!=52) {
        out={};Detail::Error(error,"unexpected CIL rewriter output size");return false;
    }
    return true;
}
} // namespace ApexCasNativeIl
