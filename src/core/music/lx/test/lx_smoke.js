/**
 * @name SmokeTest 音源
 * @description LX 引擎冒烟脚本（真实 handler：走 crypto/buffer/zlib/timer 全链路）
 * @version 1.0.0
 * @author Muyun
 * @homepage https://github.com/MyunU/Muyun-Music-Desktop
 */
const { EVENT_NAMES, request, on, send, utils } = globalThis.lx;

// 协议一致性由 LxProtocolCheck.h（--test-lxsource）逐条断言；
// 这里只验"一个真实形态的脚本能不能跑通并给出链接"。
on(EVENT_NAMES.request, ({ source, action, info }) => {
  return new Promise((resolve, reject) => {
    if (action !== 'musicUrl') return reject(new Error('unsupported action ' + action));
    const mid = info.musicInfo.songmid;
    // crypto / buffer / zlib / 宿主定时器都用一遍（任何一处缺失都会 reject）
    const sum = utils.crypto.md5('muyun');
    const nonce = utils.buffer.bufToString(utils.crypto.randomBytes(8), 'hex');
    const packed = utils.buffer.from(source + '|' + mid + '|' + sum + '|' + nonce, 'utf8');
    utils.zlib.deflate(packed).then(buf => utils.zlib.inflate(buf)).then(back => {
      const roundtrip = utils.buffer.bufToString(back, 'utf8');
      if (roundtrip !== source + '|' + mid + '|' + sum + '|' + nonce)
        return reject(new Error('zlib roundtrip mismatch'));
      // 延时一拍再 resolve：证明宿主的定时器与微任务泵能收尾
      setTimeout(() => resolve('https://smoke.example.com/play?source=' + source
                               + '&mid=' + mid + '&type=' + info.type + '&sum=' + sum), 10);
    }).catch(e => reject(e));
  });
});

send(EVENT_NAMES.inited, {
  status: true,
  name: 'SmokeTest',
  sources: {
    tx: { name: 'Smoke TX', type: 'music', actions: ['musicUrl'],
          qualitys: ['128k', '320k', 'flac', 'flac24bit'] },
    wy: { name: 'Smoke WY', type: 'music', actions: ['musicUrl'],
          qualitys: ['128k', '320k', 'flac'] }
  }
});
