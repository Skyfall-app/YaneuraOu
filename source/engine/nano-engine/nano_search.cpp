﻿#include "../../shogi.h"

#ifdef YANEURAOU_NANO_ENGINE

// -----------------------
//   やねうら王nano探索部
// -----------------------

#include <sstream>
#include <iostream>

#include "../../position.h"
#include "../../search.h"
#include "../../thread.h"
#include "../../misc.h"
#include "../../tt.h"

using namespace std;
using namespace Search;

namespace YaneuraOuNano
{
  // -----------------------
  //   指し手オーダリング
  // -----------------------

  struct MovePicker
  {
    // 通常探索から呼び出されるとき用。
    MovePicker(const Position& pos_,Move ttMove) : pos(pos_)
    {
      // 王手がかかっているなら回避手(EVASIONS)、さもなくば、すべての指し手(NON_EVASIONS)で指し手を生成する。
      if (pos.in_check())
        endMoves = generateMoves<EVASIONS>(pos, currentMoves);
      else
        endMoves = generateMoves<NON_EVASIONS>(pos, currentMoves);

      // 置換表の指し手が、この生成された集合のなかにあるなら、その先頭の指し手に置換表の指し手が来るようにしておく。
      if (ttMove != MOVE_NONE && pos.pseudo_legal(ttMove))
      {
        auto p = currentMoves;
        for (; p != endMoves;++p)
          if (*p == ttMove)
          {
            swap(*p, *currentMoves);
            break;
          }
      }
    }

    // 静止探索から呼び出される時用。
    MovePicker(const Position& pos_, Square recapSq) : pos(pos_)
    {
      // 王手がかかっているなら回避手(EVASIONS)、さもなくば、recaptureのみ生成。
      if (pos.in_check())
        endMoves = generateMoves<EVASIONS>(pos, currentMoves);
      else
      {
        // recapture以外生成しない。recapture用の指し手生成を作るべき。
        endMoves = generateMoves<CAPTURES>(pos, currentMoves);

        auto cur = currentMoves;
        while (cur != endMoves)
        {
          if (move_to(*cur)!=recapSq)
            *cur = *(--endMoves);
          else
            ++cur;
        }
      }
    }

    // 次の指し手をひとつ返す
    // 指し手が尽きればMOVE_NONEが返る。
    Move nextMove() {
      if (currentMoves == endMoves)
        return MOVE_NONE;
      return *currentMoves++;
    }

  private:
    const Position& pos;

    ExtMove moves[MAX_MOVES], *currentMoves = moves, *endMoves = moves;
  };

  // -----------------------
  //      静止探索
  // -----------------------

  Value qsearch(Position& pos, Value alpha, Value beta, Depth depth)
  {
    //cout << pos << Eval::eval(pos);
    //return Eval::eval(pos);

    // 静止探索では4手以上は延長しない。
    if (depth < -4 * ONE_PLY)
      return Eval::eval(pos);

    // 取り合いの指し手だけ生成する
    MovePicker mp(pos,move_to(pos.state()->lastMove));
    Value value;
    Move move;

    StateInfo si;
    pos.check_info_update();

    // この局面でdo_move()された合法手の数
    int moveCount = 0;

    while (move = mp.nextMove())
    {
      if (!pos.legal(move))
        continue;

      pos.do_move(move, si, pos.gives_check(move));
      value = -YaneuraOuNano::qsearch(pos, -beta, -alpha, depth - ONE_PLY);
      pos.undo_move(move);

      if (Signals.stop)
        return VALUE_ZERO;

      ++moveCount;

      if (value > alpha) // update alpha?
      {
        alpha = value;
        if (alpha >= beta)
          return alpha; // beta cut
      }
    }

    if (moveCount == 0)
    {
      // 王手がかかっているなら回避手をすべて生成しているはずで、つまりここで詰んでいたということだから
      // 詰みの評価値を返す。
      if (pos.in_check())
        return mated_in(1);

      // recaptureの指し手が尽きたということだから、評価関数を呼び出して評価値を返す。
      //cout << pos << Eval::eval(pos) << endl;
      return Eval::eval(pos);
    }

    return alpha;
  }

  // -----------------------
  //      通常探索
  // -----------------------

  // 探索しているnodeの種類
  enum NodeType { Root , PV , NonPV };

  template <NodeType NT>
  Value search(Position& pos, Value alpha, Value beta, Depth depth)
  {
    // 現在のnodeのrootからの手数。これカウンターが必要。
    // nanoだとこのカウンター持ってないので適当にごまかす。
    const int ply_from_root = (pos.this_thread()->rootDepth - depth) / ONE_PLY;

    ASSERT_LV3(alpha < beta);

    // root nodeであるか
    const bool RootNode = NT == Root;

    // PV nodeであるか(root nodeはPV nodeに含まれる)
    const bool PvNode = NT == PV || NT == Root;

    // -----------------------
    // 残り深さがないなら静止探索へ
    // -----------------------

    // 残り探索深さがなければ静止探索を呼び出して評価値を返す。
    if (depth < ONE_PLY)
      return qsearch(pos, alpha, beta, depth);

    // -----------------------
    //   置換表のprobe
    // -----------------------

    auto key = pos.state()->key();

    bool ttHit;    // 置換表がhitしたか
    TTEntry* tte = TT.probe(key, ttHit);

    // 置換表上のスコア
    // 置換表にhitしなければVALUE_NONE
    Value ttValue = ttHit ? value_from_tt(tte->value(), ply_from_root) : VALUE_NONE;

    auto thisThread = pos.this_thread();

    // 置換表の指し手
    // 置換表にhitしなければMOVE_NONE

    // RootNodeであるなら、指し手は現在注目している1手だけであるから、それが置換表にあったものとして指し手を進める。
    Move ttMove = RootNode ? thisThread->rootMoves[thisThread->PVIdx].pv[0]
      :  ttHit ? tte->move() : MOVE_NONE;

    // 置換表の値によるbeta cut
    
    if (ttHit                   // 置換表の指し手がhitして
      && tte->depth() >= depth   // 置換表に登録されている探索深さのほうが深くて
      && ttValue != VALUE_NONE   // (他スレッドからTTEntryがこの瞬間に破壊された可能性が..)
      && (ttValue >= beta && tte->bound() & BOUND_LOWER) // ttValueが下界(真の評価値はこれより大きい)もしくはジャストな値。
      )
    {
      return ttValue;
    }

    // -----------------------
    // 1手ずつ指し手を試す
    // -----------------------

    MovePicker mp(pos,ttMove);

    Value value;
    Move move;

    StateInfo si;
    pos.check_info_update();

    // この局面でdo_move()された合法手の数
    int moveCount = 0;
    Move bestMove = MOVE_NONE;

    while (move = mp.nextMove())
    {
      // root nodeでは、rootMoves()の集合に含まれていない指し手は探索をスキップする。
      if (RootNode && !std::count(thisThread->rootMoves.begin() + thisThread->PVIdx,
        thisThread->rootMoves.end(), move))
        continue;

      // legal()のチェック。root nodeだとlegal()だとわかっているのでこのチェックは不要。
      if (!RootNode && !pos.legal(move))
        continue;

      pos.do_move(move, si, pos.gives_check(move));
      value = -YaneuraOuNano::search<PV>(pos, -beta, -alpha, depth - ONE_PLY);
      pos.undo_move(move);

      // 停止シグナルが来たら置換表を汚さずに終了。
      if (Signals.stop)
        return VALUE_ZERO;

      // do_moveした指し手の数のインクリメント
      ++moveCount;

      // -----------------------
      //  root node用の特別な処理
      // -----------------------

      if (RootNode)
      {
        auto& rm = *std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);

        if (moveCount == 1 || value > alpha)
        {
          // root nodeにおいてPVの指し手または、α値を更新した場合、スコアをセットしておく。
          // (iterationの終わりでsortするのでそのときに指し手が入れ替わる。)

          rm.score = value;
          rm.pv.resize(1); // PVは変化するはずなのでいったんリセット

          // ここにPVを代入するコードを書く。(か、置換表からPVをかき集めてくるか)

        } else {

          // root nodeにおいてα値を更新しなかったのであれば、この指し手のスコアを-VALUE_INFINITEにしておく。
          // こうしておかなければ、stable_sort()しているにもかかわらず、前回の反復深化のときの値との
          // 大小比較してしまい指し手の順番が入れ替わってしまうことによるオーダリング性能の低下がありうる。
          rm.score = -VALUE_INFINITE;
        }
      }

      // -----------------------
      //  alpha値の更新処理
      // -----------------------

      if (value > alpha)
      {
        alpha = value;
        bestMove = move;

        // αがβを上回ったらbeta cut
        if (alpha >= beta)
          break;
      }

    } // end of while

    // -----------------------
    //  生成された指し手がない？
    // -----------------------
      
    // 合法手がない == 詰まされている ので、rootの局面からの手数で詰まされたという評価値を返す。
    if (moveCount == 0)
      alpha = mated_in(ply_from_root);

    // -----------------------
    //  置換表に保存する
    // -----------------------

    tte->save(key, value_to_tt(alpha, ply_from_root),
      alpha >= beta ? BOUND_LOWER : BOUND_EXACT,
      // betaを超えているということはbeta cutされるわけで残りの指し手を調べていないから真の値はまだ大きいと考えられる。
      // すなわち、このとき値は下界と考えられるから、BOUND_LOWER。
      // さもなくば、枝刈りはしていないので、これが正確な値であるはずだから、BOUND_EXACTを返す。
      depth, bestMove, VALUE_NONE,TT.generation());

    return alpha;
  }

}

using namespace YaneuraOuNano;

// --- 以下に好きなように探索のプログラムを書くべし。

// 起動時に呼び出される。時間のかからない探索関係の初期化処理はここに書くこと。
void Search::init(){}

// isreadyコマンドの応答中に呼び出される。時間のかかる処理はここに書くこと。
void  Search::clear() { TT.clear(); }

// 探索開始時に呼び出される。
// この関数内で初期化を終わらせ、slaveスレッドを起動してThread::search()を呼び出す。
// そのあとslaveスレッドを終了させ、ベストな指し手を返すこと。

void MainThread::think() {

  // 合法手がないならここで投了。
  if (rootMoves.size() == 0)
  {
    sync_cout << "bestmove " << MOVE_RESIGN << sync_endl;
    return;
  }

  // TTEntryの世代を進める。
  TT.new_search();

  rootDepth = DEPTH_ZERO;
  Value alpha,beta;
  StateInfo si;
  auto& pos = rootPos;

  // 今回に用いる思考時間 = 残り時間の1/60 + 秒読み時間
  auto us = pos.side_to_move();
  auto availableTime = Limits.time[us] / 60 + Limits.byoyomi[us];
  auto endTime = Limits.startTime + availableTime;

  // タイマースレッドを起こして、終了時間を監視させる。
  auto timerThread = new std::thread([&] {
    while (now() < endTime && !Signals.stop)
      sleep(10);
    Signals.stop = true;
  });

  // --- 反復深化のループ

  while ((rootDepth+=ONE_PLY) < MAX_PLY && !Signals.stop && (!Limits.depth || rootDepth <= Limits.depth))
  {
    // 本当はもっと探索窓を絞ったほうが効率がいいのだが…。
    alpha = -VALUE_INFINITE;
    beta = VALUE_INFINITE;

    PVIdx = 0; // MultiPVではないのでPVは1つで良い。
    
    YaneuraOuNano::search<Root>(rootPos, alpha , beta , rootDepth);

    // それぞれの指し手に対するスコアリングが終わったので並べ替えおく。
    std::stable_sort(rootMoves.begin(), rootMoves.end());

    // 読み筋を出力しておく。
    sync_cout << USI::pv(pos, rootDepth, alpha, beta) << sync_endl;
  }
  
  Move bestMove = rootMoves.at(0).pv[0];
  sync_cout << "bestmove " << bestMove << sync_endl;

  Signals.stop = true;
  timerThread->join();
  delete timerThread;
}

// 探索本体。並列化している場合、ここがslaveのエントリーポイント。
void Thread::search(){}

#endif // YANEURAOU_NANO_ENGINE
