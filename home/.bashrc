#
# ~/.bashrc
#

# If not running interactively, don't do anything
[[ $- != *i* ]] && return

alias ls='ls --color=auto'
alias grep='grep --color=auto'
PS1='[\u@\h \W]\$ '

. "$HOME/.local/bin/env"
export PATH="$HOME/.npm-global/bin:$PATH"


# Added by Antigravity CLI installer
export PATH="/home/doki/.local/bin:$PATH"

# kimi-code
export PATH="/home/doki/.kimi-code/bin:$PATH"
