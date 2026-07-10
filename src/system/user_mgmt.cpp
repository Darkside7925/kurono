#include "user_mgmt.h"
#include "../fs/kvfs.h"
#include "../drivers/rtc.h"
#include "../drivers/serial.h"

User UserManager::users[UserManager::MAX_USERS];
int  UserManager::user_count = 0;
int  UserManager::current_user = -1;

// ---------------------------------------------------------------- helpers
static int   um_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void  um_cpy(char* d, const char* s, int max) {
    int i = 0; if (!d || max < 1) return;
    while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static bool  um_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}
static bool  um_is_letter(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static bool  um_is_digit(char c)  { return c>='0'&&c<='9'; }
static bool  um_is_lower(char c)  { return c>='a'&&c<='z'; }
static bool  um_is_upper(char c)  { return c>='A'&&c<='Z'; }
static bool  um_is_special(char c){
    return c=='!'||c=='@'||c=='#'||c=='$'||c=='%'||c=='^'||c=='&'||c=='*'
        || c=='('||c==')'||c=='-'||c=='_'||c=='='||c=='+'||c=='['||c==']'
        || c=='{'||c=='}'||c=='|'||c==';'||c==':'||c==','||c=='.'||c=='?'
        || c=='/'||c=='<'||c=='>'||c=='~';
}

// ----------------------------------------------------------------- SHA256
struct um_sha256 { uint32_t s[8]; uint64_t bits; uint8_t buf[64]; int buf_len; };
static const uint32_t UM_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static uint32_t um_rrot(uint32_t x, int n){ return (x>>n)|(x<<(32-n)); }
static void um_compress(um_sha256* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i=16;i<64;i++){
        uint32_t s0=um_rrot(w[i-15],7)^um_rrot(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=um_rrot(w[i-2],17)^um_rrot(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for (int i=0;i<64;i++){
        uint32_t S1=um_rrot(e,6)^um_rrot(e,11)^um_rrot(e,25);
        uint32_t ch=(e&f)^((~e)&g);
        uint32_t t1=h+S1+ch+UM_K[i]+w[i];
        uint32_t S0=um_rrot(a,2)^um_rrot(a,13)^um_rrot(a,22);
        uint32_t mj=(a&b)^(a&cc)^(b&cc);
        uint32_t t2=S0+mj;
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void um_init(um_sha256* c){
    static const uint32_t H[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    for(int i=0;i<8;i++) c->s[i]=H[i];
    c->bits=0; c->buf_len=0;
}
static void um_update(um_sha256* c, const uint8_t* d, int n){
    c->bits += (uint64_t)n*8u;
    while (n>0){
        int t = 64-c->buf_len; if (t>n) t=n;
        for (int i=0;i<t;i++) c->buf[c->buf_len+i]=d[i];
        c->buf_len+=t; d+=t; n-=t;
        if (c->buf_len==64){ um_compress(c,c->buf); c->buf_len=0; }
    }
}
static void um_final(um_sha256* c, uint8_t out[32]){
    c->buf[c->buf_len++]=0x80;
    if (c->buf_len>56){ while(c->buf_len<64) c->buf[c->buf_len++]=0; um_compress(c,c->buf); c->buf_len=0; }
    while (c->buf_len<56) c->buf[c->buf_len++]=0;
    uint64_t bits=c->bits;
    for (int i=7;i>=0;i--) c->buf[c->buf_len++] = (uint8_t)(bits>>(i*8));
    um_compress(c,c->buf);
    for (int i=0;i<8;i++){
        out[i*4]   = (uint8_t)(c->s[i]>>24);
        out[i*4+1] = (uint8_t)(c->s[i]>>16);
        out[i*4+2] = (uint8_t)(c->s[i]>>8);
        out[i*4+3] = (uint8_t)c->s[i];
    }
}
static void um_hex(uint8_t* in, int n, char* out){
    static const char* H="0123456789abcdef";
    for (int i=0;i<n;i++){ out[i*2]=H[(in[i]>>4)&0xF]; out[i*2+1]=H[in[i]&0xF]; }
    out[n*2]=0;
}

// --------------------------------------------------------- crypto wrappers
void UserManager::HashPassword(const char* salt, const char* plaintext, char* out_hex_64){
    um_sha256 c; um_init(&c);
    if (salt) um_update(&c, (const uint8_t*)salt, um_strlen(salt));
    if (plaintext) um_update(&c, (const uint8_t*)plaintext, um_strlen(plaintext));
    uint8_t d[32]; um_final(&c, d);
    um_hex(d, 32, out_hex_64);
}

void UserManager::GenerateSalt(char* out_hex_16){
    // Combine RTC seconds, RDTSC, and a process counter.  Hash and take 8
    // bytes = 16 hex chars.  Real entropy good enough for password salting
    // (we never expose the plaintext password).
    uint64_t tsc = 0;
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    tsc = ((uint64_t)hi << 32) | lo;
    RTC::Date rd; RTC::Time rt; RTC::ReadDateTime(rd, rt);
    uint64_t mix = tsc ^ ((uint64_t)rd.year << 48) ^ ((uint64_t)rd.mon << 40)
                       ^ ((uint64_t)rd.dom << 32)  ^ ((uint64_t)rt.h << 24)
                       ^ ((uint64_t)rt.m << 16)    ^ ((uint64_t)rt.s << 8);
    static uint64_t counter = 0;
    counter += 0x9E3779B97F4A7C15ull; mix ^= counter;

    um_sha256 c; um_init(&c);
    um_update(&c, (const uint8_t*)&mix, sizeof(mix));
    uint8_t d[32]; um_final(&c, d);
    um_hex(d, 8, out_hex_16);
}

// ---------------------------------------------------- /etc/passwd persistence
static int um_putc(char* b, int p, int m, char c){ if (p<m-1){ b[p++]=c; b[p]=0; } return p; }
static int um_puts(char* b, int p, int m, const char* s){ while (s && *s && p<m-1) b[p++]=*s++; if (p<m) b[p]=0; return p; }
static int um_putu(char* b, int p, int m, uint32_t v){
    if (v==0) return um_putc(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0){ t[ti++]=(char)('0'+(v%10)); v/=10; }
    while (ti>0) p=um_putc(b,p,m,t[--ti]);
    return p;
}

void UserManager::PersistToDisk(){
    // /etc/passwd  -- one line per user, colon-separated:
    //   username:x:uid:gid:display_name:/home/user:/bin/bash
    // /etc/shadow  -- username:salt$hash:0:0:99999:7:::
    // /etc/kurono.conf -- per-user prefs
    KVFS::Mkdirs("/etc");
    char pwd[2048]; int pp = 0;
    char sh[2048];  int sp = 0;
    char kc[2048];  int kp = 0;
    kp = um_puts(kc, kp, 2048, "# Kurono per-user configuration\n");
    for (int i=0;i<user_count;i++){
        const User& u = users[i];
        pp = um_puts(pwd, pp, 2048, u.username);
        pp = um_puts(pwd, pp, 2048, ":x:");
        pp = um_putu(pwd, pp, 2048, (uint32_t)(1000+i));
        pp = um_putc(pwd, pp, 2048, ':');
        pp = um_putu(pwd, pp, 2048, (uint32_t)(1000+i));
        pp = um_putc(pwd, pp, 2048, ':');
        pp = um_puts(pwd, pp, 2048, u.display_name[0]?u.display_name:u.username);
        pp = um_puts(pwd, pp, 2048, ":/home/");
        pp = um_puts(pwd, pp, 2048, u.username);
        pp = um_puts(pwd, pp, 2048, ":/bin/bash\n");

        sp = um_puts(sh, sp, 2048, u.username);
        sp = um_puts(sh, sp, 2048, ":");
        sp = um_puts(sh, sp, 2048, u.salt);
        sp = um_puts(sh, sp, 2048, "$");
        sp = um_puts(sh, sp, 2048, u.password_hash);
        sp = um_puts(sh, sp, 2048, ":0:0:99999:7:::\n");

        kp = um_puts(kc, kp, 2048, "[user.");
        kp = um_puts(kc, kp, 2048, u.username);
        kp = um_puts(kc, kp, 2048, "]\n");
        kp = um_puts(kc, kp, 2048, "display_name=");
        kp = um_puts(kc, kp, 2048, u.display_name);
        kp = um_puts(kc, kp, 2048, "\navatar_id=");
        kp = um_putu(kc, kp, 2048, (uint32_t)(u.avatar_id<0?0:u.avatar_id));
        kp = um_puts(kc, kp, 2048, "\naccent_color=");
        kp = um_putu(kc, kp, 2048, u.accent_color);
        kp = um_puts(kc, kp, 2048, "\nauto_login=");
        kp = um_puts(kc, kp, 2048, u.auto_login ? "1" : "0");
        kp = um_puts(kc, kp, 2048, "\ntimezone=");
        kp = um_puts(kc, kp, 2048, u.timezone[0]?u.timezone:"UTC");
        kp = um_puts(kc, kp, 2048, "\nlanguage=");
        kp = um_puts(kc, kp, 2048, u.language[0]?u.language:"en_US");
        kp = um_puts(kc, kp, 2048, "\nis_admin=");
        kp = um_puts(kc, kp, 2048, u.is_admin ? "1" : "0");
        kp = um_puts(kc, kp, 2048, "\n\n");
    }
    KVFS::WriteString("/etc/passwd", pwd);
    KVFS::WriteString("/etc/shadow", sh);
    KVFS::WriteString("/etc/kurono.conf", kc);
}

// ---------------------------------------------------------------------- API
void UserManager::Init(){
    user_count = 0;
    current_user = -1;
    // Load /etc/shadow if present (real persistence path).  We don't try
    // to recover plaintext passwords -- only the salt+hash pair, which is
    // exactly what authentication needs.
    char buf[4096];
    int n = KVFS::ReadFile("/etc/shadow", buf, sizeof(buf)-1);
    if (n <= 0) return;
    buf[n] = 0;
    int i = 0;
    while (buf[i] && user_count < MAX_USERS){
        // username : salt $ hash : ...
        char uname[32] = {0}; int up = 0;
        while (buf[i] && buf[i] != ':' && up < 31) { uname[up++] = buf[i++]; }
        if (buf[i] != ':') break;
        i++;
        char salt[24] = {0}; int sp = 0;
        while (buf[i] && buf[i] != '$' && buf[i] != ':' && sp < 23) { salt[sp++] = buf[i++]; }
        char hash[80] = {0}; int hp = 0;
        if (buf[i] == '$') {
            i++;
            while (buf[i] && buf[i] != ':' && hp < 79) { hash[hp++] = buf[i++]; }
        }
        // skip rest of line
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;

        if (uname[0] && hash[0]) {
            User& u = users[user_count++];
            for (int j=0;j<(int)sizeof(User);j++) ((char*)&u)[j] = 0;
            um_cpy(u.username, uname, 32);
            um_cpy(u.display_name, uname, 48);
            um_cpy(u.salt, salt, 24);
            um_cpy(u.password_hash, hash, 80);
            u.avatar_id = user_count - 1;
            u.accent_color = 0xFF5C8AFF;
            u.is_admin = (user_count == 1);
            um_cpy(u.timezone, "UTC", 32);
            um_cpy(u.language, "en_US", 16);
        }
    }
    SerialLogger::Log("UserManager: loaded ");
    SerialLogger::LogDec(user_count);
    SerialLogger::Log(" user(s) from /etc/shadow\r\n");
}

bool UserManager::Login(const char* username, const char* password){
    User* u = FindByName(username);
    if (!u) return false;
    char hash[80];
    HashPassword(u->salt, password, hash);
    if (!um_eq(hash, u->password_hash)) return false;
    current_user = (int)(u - users);
    return true;
}

bool UserManager::LoginByPin(const char* username, const char* pin){
    User* u = FindByName(username);
    if (!u || !u->has_pin) return false;
    char hash[80];
    HashPassword(u->salt, pin, hash);
    if (!um_eq(hash, u->pin_hash)) return false;
    current_user = (int)(u - users);
    return true;
}

void UserManager::Logout(){ current_user = -1; }

User* UserManager::FindByName(const char* username){
    for (int i=0;i<user_count;i++) if (um_eq(users[i].username, username)) return &users[i];
    return nullptr;
}

bool UserManager::AddUser(const char* username, const char* password){
    User u; for (int i=0;i<(int)sizeof(User);i++) ((char*)&u)[i] = 0;
    um_cpy(u.username, username, 32);
    um_cpy(u.display_name, username, 48);
    u.avatar_id = user_count;
    u.accent_color = 0xFF5C8AFF;
    u.is_admin = (user_count == 0);
    um_cpy(u.timezone, "UTC", 32);
    um_cpy(u.language, "en_US", 16);
    return RegisterUser(u, password);
}

bool UserManager::RegisterUser(const User& tmpl, const char* plaintext_password){
    if (user_count >= MAX_USERS) return false;
    if (!IsUsernameValid(tmpl.username)) return false;
    if (IsUsernameTaken(tmpl.username))  return false;
    User& u = users[user_count];
    u = tmpl;
    GenerateSalt(u.salt);
    HashPassword(u.salt, plaintext_password, u.password_hash);
    if (u.display_name[0] == 0) um_cpy(u.display_name, u.username, 48);
    if (u.timezone[0] == 0)     um_cpy(u.timezone, "UTC", 32);
    if (u.language[0] == 0)     um_cpy(u.language, "en_US", 16);
    if (u.accent_color == 0)    u.accent_color = 0xFF5C8AFF;
    user_count++;

    // Create home directory
    char home[96]; int hp = 0;
    hp = um_puts(home, hp, 96, "/home/");
    hp = um_puts(home, hp, 96, u.username);
    KVFS::Mkdirs(home);

    PersistToDisk();
    return true;
}

bool UserManager::RemoveUser(const char* username){
    for (int i=0;i<user_count;i++){
        if (um_eq(users[i].username, username)){
            for (int j=i;j<user_count-1;j++) users[j] = users[j+1];
            user_count--;
            if (current_user == i) current_user = -1;
            else if (current_user > i) current_user--;
            PersistToDisk();
            return true;
        }
    }
    return false;
}

const char* UserManager::GetCurrentUsername(){
    if (current_user >= 0 && current_user < user_count) return users[current_user].username;
    return "user";
}
const char* UserManager::GetCurrentDisplayName(){
    if (current_user >= 0 && current_user < user_count) {
        const char* d = users[current_user].display_name;
        return d[0] ? d : users[current_user].username;
    }
    return "User";
}
int UserManager::GetCurrentUserIndex(){ return current_user; }
int UserManager::GetUserCount(){ return user_count; }

int UserManager::MeasurePassword(const char* p){
    if (!p) return PWD_WEAK;
    int len = um_strlen(p);
    int classes = 0;
    bool lower=false, upper=false, digit=false, spec=false;
    for (int i=0;i<len;i++){
        if (um_is_lower(p[i])) lower = true;
        else if (um_is_upper(p[i])) upper = true;
        else if (um_is_digit(p[i])) digit = true;
        else if (um_is_special(p[i])) spec = true;
    }
    if (lower) classes++; if (upper) classes++; if (digit) classes++; if (spec) classes++;
    int score = 0;
    if (len >= 6) score++;
    if (len >= 10) score++;
    if (len >= 14) score++;
    if (classes >= 2) score++;
    if (classes >= 3) score++;
    if (classes == 4) score++;
    if (score <= 1) return PWD_WEAK;
    if (score <= 3) return PWD_FAIR;
    if (score <= 4) return PWD_STRONG;
    return PWD_VERY_STRONG;
}

bool UserManager::IsUsernameValid(const char* u){
    int n = um_strlen(u);
    if (n < 3 || n > 31) return false;
    if (!um_is_letter(u[0])) return false;
    for (int i=0;i<n;i++){
        char c = u[i];
        if (um_is_letter(c) || um_is_digit(c) || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

bool UserManager::IsUsernameTaken(const char* u){
    return FindByName(u) != nullptr;
}
