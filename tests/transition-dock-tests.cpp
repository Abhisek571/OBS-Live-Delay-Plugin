#include "active-delay-dock.hpp"
#include <obs-module.h>
#include <QApplication>
#include <QPushButton>
#include <QMessageBox>
#include <QTimer>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
namespace { std::map<std::string,std::string> locale; }
extern "C" const char *obs_module_text(const char *key) {
 auto it=locale.find(key);return it==locale.end()?key:it->second.c_str();
}
int main(int argc,char **argv){
 QApplication app(argc,argv);
 try{
  if(argc!=2)throw std::runtime_error("locale fixture argument required");
  std::ifstream file(argv[1]);std::string line;
  while(std::getline(file,line)){
   auto eq=line.find('=');if(eq==std::string::npos)continue;
   auto start=line.find('"',eq),end=line.rfind('"');
   if(start!=std::string::npos && end>start)locale.emplace(line.substr(0,eq),line.substr(start+1,end-start-1));
  }
  auto session=std::make_shared<active_delay::ActiveDelaySession>();
  // No frontend callbacks, OBS application, profile configuration, or outputs.
  active_delay::ActiveDelayDock dock(session,nullptr);
  auto *live=dock.findChild<QPushButton*>("ald_return_live");
  auto *dump=dock.findChild<QPushButton*>("ald_emergency_dump");
  auto *delay=dock.findChild<QPushButton*>("ald_start_delay");
  if(!live || !dump || !delay)throw std::runtime_error("all real dock transition controls must exist");
  QApplication::processEvents();
  auto *arm=dock.findChild<QPushButton*>("ald_arm_buffer");
  auto *start=dock.findChild<QPushButton*>("ald_start_broadcast");
  if(!arm || !arm->isEnabled() || !start || start->isEnabled())
   throw std::runtime_error("stopped dock must offer Arm Buffer, never Start before ready");
  if(live->isEnabled() || dump->isEnabled() || delay->isEnabled())throw std::runtime_error("transition controls must remain disabled without owner readiness");
  if(!dump->toolTip().contains("cannot be recalled") || !dump->toolTip().contains("retain"))
   throw std::runtime_error("dump help must describe discard/rebuild and irreversibility");
  session->controller.delay.set_target(std::chrono::seconds(7));
  dock.return_live();
  if(session->controller.delay.status().target_delay!=std::chrono::seconds(7))throw std::runtime_error("Return Live must route through owner, never independently mutate controller");
  bool inspected=false;
  QTimer::singleShot(0,[&]{
   for(auto *widget:QApplication::topLevelWidgets())if(auto *box=qobject_cast<QMessageBox*>(widget)){
    inspected=box->defaultButton()==box->button(QMessageBox::No) && box->text().contains("cannot be recalled") && box->text().contains("not a partial dump");
    box->done(QMessageBox::No);
   }
  });
  dock.emergency_dump();
  if(!inspected || session->controller.delay.status().target_delay!=std::chrono::seconds(7))
   throw std::runtime_error("real dump confirmation must default to No and cancellation must preserve target");
  dock.shutdown();
  std::cout<<"Isolated real dock control/confirmation tests passed\n";
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
