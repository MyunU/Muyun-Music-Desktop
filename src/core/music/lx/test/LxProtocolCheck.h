#pragma once

/**
 * LX 音源脚本协议一致性断言脚本（内置 JS 源码）
 *
 * 用途：把官方自定义源协议逐条变成可执行断言——缺哪个 API、哪个签名不对、
 * 哪个行为不符合（比如 Promise 返回值、NoPadding、zlib 返回 Promise）都会
 * 出现在 `__protocolReport` 里，自检 `--test-lxsource` 直接判 FAIL。
 *
 * 结论通过 `lx.send(EVENT_NAMES.inited, { ..., __protocolReport, __protocolChecks })`
 * 回传给宿主（引擎把 inited 的入参原样存在 scriptInfo 里）。
 *
 * 注意：本文件是 C++ 头（raw string），改 JS 时别把结束分隔符（右括号 + LXJS + 双引号）
 * 写进脚本文本里，否则字符串会提前截断。
 */
namespace Muyun {
namespace LxTest {

static const char *kProtocolCheckScript = R"LXJS(/**
 * @name 暮云音乐 LX 协议一致性自检
 * @description 逐条断言官方自定义源协议里的 API 与行为
 * @version 1.0.0
 * @author Muyun
 * @homepage https://github.com/MyunU/Muyun-Music-Desktop
 */
var LX_PROBLEMS = [];
var LX_CHECKS = 0;
function lxNeed(cond, msg) {
  LX_CHECKS++;
  if (!cond) LX_PROBLEMS.push(msg);
}

// ---------- 1) globalThis.lx 顶层 ----------
lxNeed(typeof lx === 'object' && lx !== null, 'globalThis.lx 缺失');
lxNeed(lx.env === 'desktop', 'lx.env 应为 desktop，实际 ' + lx.env);
lxNeed(typeof lx.version === 'string' && lx.version.length > 0, 'lx.version 缺失');
lxNeed(lx.EVENT_NAMES && lx.EVENT_NAMES.request === 'request'
       && lx.EVENT_NAMES.inited === 'inited'
       && lx.EVENT_NAMES.updateAlert === 'updateAlert', 'EVENT_NAMES 取值不正确');
lxNeed(typeof lx.on === 'function', 'lx.on 缺失');
lxNeed(typeof lx.send === 'function', 'lx.send 缺失');
lxNeed(typeof lx.request === 'function', 'lx.request 缺失');
lxNeed(typeof lx.utils === 'object', 'lx.utils 缺失');

// ---------- 2) currentScriptInfo（导入时头部注释解析到的真值） ----------
var ci = lx.currentScriptInfo || {};
lxNeed(ci.name === '暮云音乐 LX 协议一致性自检', 'currentScriptInfo.name 不对：' + ci.name);
lxNeed(ci.description && ci.description.length > 0, 'currentScriptInfo.description 空');
lxNeed(ci.version === '1.0.0', 'currentScriptInfo.version 不对：' + ci.version);
lxNeed(ci.author === 'Muyun', 'currentScriptInfo.author 不对：' + ci.author);
lxNeed(typeof ci.homepage === 'string' && ci.homepage.length > 0, 'currentScriptInfo.homepage 空');
lxNeed(typeof ci.rawScript === 'string' && ci.rawScript.indexOf('@description') >= 0,
       'currentScriptInfo.rawScript 不是原始代码');

// ---------- 3) lx.utils.crypto ----------
var C = lx.utils.crypto || {};
lxNeed(typeof C.md5 === 'function', 'crypto.md5 缺失');
lxNeed(typeof C.aesEncrypt === 'function', 'crypto.aesEncrypt 缺失（旧名 aesEn 不算）');
lxNeed(typeof C.rsaEncrypt === 'function', 'crypto.rsaEncrypt 缺失');
lxNeed(typeof C.randomBytes === 'function', 'crypto.randomBytes 缺失');
lxNeed(C.md5('abc') === '900150983cd24fb0d6963f7d28e17f72', 'crypto.md5 结果不对');

// AES 用 NIST SP800-38A 向量钉死"参数顺序 + 算法 + 填充"三件事
var B = lx.utils.buffer;
var nistKey = B.from('2b7e151628aed2a6abf7158809cf4f3c', 'hex');
var nistIv = B.from('000102030405060708090a0b0c0d0e0f', 'hex');
var nistPt = B.from('6bc1bee22e409f96e93d7e117393172a', 'hex');
var cbc = C.aesEncrypt(nistPt, 'aes-128-cbc', nistKey, nistIv);
lxNeed(cbc && cbc.length === 32, 'aes-128-cbc 应为 PKCS7（16 字节进 → 32 字节出）');
lxNeed(B.bufToString(cbc, 'hex').substring(0, 32) === '7649abac8119b246cee98e9b12e9197d',
       'aes-128-cbc 结果与 NIST 向量不符（参数顺序或实现有误）：' + B.bufToString(cbc, 'hex'));
var ecb = C.aesEncrypt(B.from('00112233445566778899aabbccddeeff', 'hex'),
                       'aes-128-ecb', B.from('000102030405060708090a0b0c0d0e0f', 'hex'), '');
lxNeed(ecb && ecb.length === 16, 'aes-128-ecb 应为 NoPadding（16 字节进 → 16 字节出）');
lxNeed(B.bufToString(ecb, 'hex') === '69c4e0d86a7b0430d8cdb78070b4c55a',
       'aes-128-ecb NoPadding 结果与 NIST 向量不符：' + B.bufToString(ecb, 'hex'));

var rb = C.randomBytes(16);
lxNeed(rb && rb.length === 16, 'randomBytes(16) 长度不对：' + (rb && rb.length));
lxNeed(B.bufToString(rb, 'hex').length === 32, 'randomBytes 结果无法转 hex');
lxNeed(C.randomBytes(8).length === 8, 'randomBytes(8) 长度不对');

var rsaKey = '-----BEGIN PUBLIC KEY-----\n'
  + 'MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUrX/aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK9snQXE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45wIDAQAB\n'
  + '-----END PUBLIC KEY-----';
var rsaOut = C.rsaEncrypt(B.from('muyun'), rsaKey);
lxNeed(rsaOut && rsaOut.length === 128,
       'crypto.rsaEncrypt 应返回 128 字节二进制（NoPadding），实际 ' + (rsaOut && rsaOut.length));

// ---------- 4) lx.utils.buffer ----------
lxNeed(typeof B.from === 'function', 'buffer.from 缺失');
lxNeed(typeof B.bufToString === 'function', 'buffer.bufToString 缺失');
lxNeed(B.from('muyun').toString('base64') === 'bXV5dW4=', 'buffer.from(utf8).toString(base64) 不对');
lxNeed(B.bufToString(B.from('bXV5dW4=', 'base64'), 'utf8') === 'muyun', 'base64 → utf8 往返不一致');
lxNeed(B.bufToString(B.from('6d7579756e', 'hex'), 'utf8') === 'muyun', 'hex → utf8 不对');
lxNeed(B.bufToString(B.from([109, 117, 121, 117, 110]), 'utf8') === 'muyun', '数组 → utf8 不对');
lxNeed(B.from('muyun').length === 5, 'buffer.length 不对');
lxNeed(typeof Buffer !== 'undefined' && Buffer.isBuffer(B.from('x')), '全局 Buffer.isBuffer 认不出 Buffer');
lxNeed(Buffer.from('AA==', 'base64').length === 1, '全局 Buffer.from(base64) 不对');
lxNeed(Buffer.concat([B.from('mu'), B.from('yun')]).toString('utf8') === 'muyun', 'Buffer.concat 不对');

// ---------- 5) lx.utils.zlib（返回 Promise<Buffer>） ----------
lxNeed(typeof lx.utils.zlib === 'object', 'lx.utils.zlib 缺失');
lxNeed(typeof lx.utils.zlib.inflate === 'function', 'zlib.inflate 缺失');
lxNeed(typeof lx.utils.zlib.deflate === 'function', 'zlib.deflate 缺失');
var dp = lx.utils.zlib.deflate(B.from('muyun-zlib'));
lxNeed(dp && typeof dp.then === 'function', 'zlib.deflate 必须返回 Promise');

// ---------- 6) 宿主 API ----------
lxNeed(typeof setTimeout === 'function', 'setTimeout 缺失');
lxNeed(typeof clearTimeout === 'function', 'clearTimeout 缺失');
lxNeed(typeof console === 'object' && typeof console.log === 'function', 'console.log 缺失');

// ---------- 7) on/send 返回 Promise ----------
var onRet = lx.on(lx.EVENT_NAMES.request, function (data) {
  return new Promise(function (resolve) {
    lxNeed(data && data.source === 'tx', 'request 事件 source 不对：' + (data && data.source));
    lxNeed(data.action === 'musicUrl', 'request 事件 action 不对：' + (data && data.action));
    lxNeed(data.info && data.info.musicInfo && data.info.musicInfo.songmid === 'MID123',
           'request 事件 musicInfo 不完整');
    lxNeed(data.info.type === '320k', 'request 事件 type 应为协议音质 320k');
    // 特意在定时器里 resolve：证明宿主定时器 + 微任务泵能把 Promise 收干净；
    // 顺带把"宿主传进来的数字是不是 JS number"带回去（C++→JS 数字退化成字符串
    // 会让脚本的 ===260 / !==200 这类严格比较全错，四-61 真踩过）
    setTimeout(function () {
      resolve('https://proto.example.com/play?mid=' + data.info.musicInfo.songmid
              + '&dur=' + (typeof data.info.musicInfo.duration)
              + ':' + (data.info.musicInfo.duration === 260));
    }, 5);
  });
});
lxNeed(onRet && typeof onRet.then === 'function', 'lx.on 应返回 Promise');

// ---------- 异步阶段 ----------
var timerFired = false;
setTimeout(function (a, b) { if (a === 1 && b === 'x') timerFired = true; }, 5, 1, 'x');
var clearedFired = false;
var clearId = setTimeout(function () { clearedFired = true; }, 5);
clearTimeout(clearId);

function lxFinish() {
  // 再等一拍才收尾：微任务链可能比 5ms 定时器先跑完，那样 timerFired 还是 false（假失败）。
  // 这一拍本身也是断言——延时 40ms 的回调真能触发，才说明宿主把定时器接进了泵。
  setTimeout(function () {
    lxNeed(timerFired, 'setTimeout 回调没有触发（或透传参数丢失）');
    lxNeed(!clearedFired, 'clearTimeout 没有取消掉定时器');
    var cancelRet = lx.request('http://127.0.0.1:9/', { method: 'get', timeout: 800 },
                               function (err, resp, body) {});
    lxNeed(typeof cancelRet === 'function', 'lx.request 应返回取消函数');
    // sources 里故意塞非法值：宿主按协议过滤后只该留下合法音质/源
    lx.send(lx.EVENT_NAMES.inited, {
      sources: {
        tx: { name: '协议自检', type: 'music', actions: ['musicUrl', 'lyric'],
              qualitys: ['128k', 'master', 'flac', 'flac24bit'] },
        xm: { name: '非法源', type: 'music', actions: ['musicUrl'], qualitys: ['128k'] },
        local: { name: '本地', type: 'music', actions: ['musicUrl', 'lyric', 'pic'], qualitys: [] }
      },
      __protocolReport: LX_PROBLEMS,
      __protocolChecks: LX_CHECKS
    });
  }, 40);
}

lx.utils.zlib.deflate(B.from('muyun-zlib'))
  .then(function (z) {
    lxNeed(z && z.length > 0, 'zlib.deflate 结果为空');
    return lx.utils.zlib.inflate(z);
  })
  .then(function (back) {
    lxNeed(B.bufToString(back, 'utf8') === 'muyun-zlib',
           'zlib deflate→inflate 往返不一致：' + B.bufToString(back, 'utf8'));
    return lx.utils.zlib.inflate(B.from('not-a-zlib-stream')).then(
      function () { lxNeed(false, 'zlib.inflate 对坏数据竟然成功'); },
      function () {});
  })
  .then(lxFinish, function (e) {
    lxNeed(false, '异步阶段异常：' + (e && e.message ? e.message : e));
    lxFinish();
  });
)LXJS";

} // namespace LxTest
} // namespace Muyun
