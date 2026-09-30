/*
 * fanyi 扩展 — sites.js
 * 视频字幕站点选择器配置表：主机名（后缀匹配）→ 字幕文本元素选择器列表。
 * 站点改版后在此更新即可；未列出的站点走通用「视频覆盖层文本」检测。
 */
window.__fanyiSites = {
  'youtube.com': ['.ytp-caption-segment'],
  'm.youtube.com': ['.ytp-caption-segment'],
  'bilibili.com': ['.bpx-player-subtitle-text', '.bilibili-player-video-subtitle-text'],
  'netflix.com': ['.player-timedtext-text-container span'],
  'iqiyi.com': ['.iqp-player-subtitle', '.iqp-txt-wrapper'],
  'youku.com': ['.subtitle-text', '.kui-subtitle-page'],
  'v.qq.com': ['.txp-subtitle-text'],
  'vip.1905.com': ['.v-subtitle-text'],
  'coursera.org': ['.video__caption', '.rc-Phrase'],
  'udemy.com': ['.well--container--pFYwq', '.captions-display--captions-cued--358Xq'],
  'ted.com': ['.p_breadcrumb', 'h2.talk-transcript__teaser'],
};
